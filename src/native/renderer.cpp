// thebigs2 - native renderer: drawing through Plume (see renderer.h).
//
// The draw path follows the SDK's Vulkan backend (VulkanCommandProcessor::
// IssueDraw, UpdateSystemConstantValues, UpdateBindings, VulkanPipelineCache)
// for its host render target path, minus everything EDRAM: shaders, pipeline
// state, system constants and bindings are what the SDK would use, so the
// translated shaders see the same inputs as in the emulator. Only Plume's
// generic interface is used; the SDK device answers the capability questions
// (format features, limits).

#include "renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/instance.h>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include "plume_device.h"
#include "shaders.h"
#include "textures.h"

namespace tb2::native {

namespace {

#include "glsl/pack_depth_ms_ps.h"
#include "glsl/pack_depth_ps.h"
#include "glsl/present_fxaa_ps.h"
#include "glsl/present_gamma_table_ps.h"
#include "glsl/present_vs.h"
#include "glsl/resolve_ms_ps.h"
#include "glsl/resolve_ps.h"

namespace pl = plume;
namespace xg = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using rex::graphics::RegisterFile;
using rex::ui::vulkan::VulkanDevice;

// Per-frame arenas. Most used (2026-10-20, gameplay): 19 MB vertex data, 7 MB
// constants, 96 MB uploads in the first match frame. Each stays below the
// 246 MB device-local + host-visible heap of GPUs without resizable BAR, where
// VMA tries upload buffers first.
constexpr uint64_t kVertexArenaSize = 128u << 20;   // vertex + index data
constexpr uint64_t kUniformArenaSize = 64u << 20;   // constants
constexpr uint64_t kStagingArenaSize = 128u << 20;  // texture uploads
// EDRAM surfaces have no height: render targets get the rows draws and
// resolves reach (guest pixels, rounded up), up to the guest's 2048.
constexpr uint32_t kRenderTargetRowsMax = 2048, kRenderTargetRowsStep = 64;
constexpr uint32_t kResolveStamps = 64;  // GPU timestamps after resolves per frame
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr pl::RenderBarrierStages kAllStages = pl::RenderBarrierStage::ALL;

constexpr uint32_t kColorInfoRegs[4] = {xg::XE_GPU_REG_RB_COLOR_INFO, xg::XE_GPU_REG_RB_COLOR1_INFO,
                                        xg::XE_GPU_REG_RB_COLOR2_INFO, xg::XE_GPU_REG_RB_COLOR3_INFO};
constexpr uint32_t kBlendControlRegs[4] = {
    xg::XE_GPU_REG_RB_BLENDCONTROL0, xg::XE_GPU_REG_RB_BLENDCONTROL1, xg::XE_GPU_REG_RB_BLENDCONTROL2,
    xg::XE_GPU_REG_RB_BLENDCONTROL3};

// --- Small helpers -----------------------------------------------------------

// CPU writes to guest memory, per 4 KB page, as write epochs: through the
// SDK's physical memory invalidation callbacks (one-shot write protection, as
// the emulator's shared memory uses). Data read after Watch() returned epoch e
// is unchanged while every page's last write is <= e. Without guest memory
// attached nothing counts as unchanged.
class PageWatch {
 public:
  static constexpr uint32_t kPages = 0x20000000u >> 12;

  // Never destroyed: the SDK may call back until the process ends.
  static PageWatch& Get() {
    static PageWatch* watch = new PageWatch;
    return *watch;
  }
  void Attach(rex::memory::Memory* memory) {
    if (memory_ || !memory) return;
    memory_ = memory;
    memory->RegisterPhysicalMemoryInvalidationCallback(Invalidate, this);
  }
  bool attached() const { return memory_ != nullptr; }

  // Call before reading the memory; returns the epoch to check against.
  uint64_t Watch(uint32_t address, uint32_t bytes) {
    const uint64_t epoch = epoch_.load(std::memory_order_acquire);
    if (memory_ && bytes && address + uint64_t(bytes) <= 0x20000000)
      memory_->EnablePhysicalMemoryAccessCallbacks(address, bytes, true, false);
    return epoch;
  }
  bool Unchanged(uint32_t address, uint32_t bytes, uint64_t since) const {
    if (!memory_ || !since || !bytes || address + uint64_t(bytes) > 0x20000000) return false;
    for (uint32_t page = address >> 12, last = (address + bytes - 1) >> 12; page <= last; ++page)
      if (written_[page].load(std::memory_order_relaxed) > since) return false;
    return true;
  }

 private:
  static std::pair<uint32_t, uint32_t> Invalidate(void* context, uint32_t address,
                                                  uint32_t length, bool) {
    auto& self = *static_cast<PageWatch*>(context);
    if (length && address < 0x20000000) {
      const uint64_t epoch = self.epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
      const uint32_t last =
          uint32_t(std::min<uint64_t>(uint64_t(address) + length - 1, 0x1FFFFFFF) >> 12);
      for (uint32_t page = address >> 12; page <= last; ++page)
        self.written_[page].store(epoch, std::memory_order_relaxed);
    }
    // Only the pages reported here may be unwatched: others stay protected.
    return {address, length};
  }

  rex::memory::Memory* memory_ = nullptr;
  std::atomic<uint64_t> epoch_{1};
  std::unique_ptr<std::atomic<uint64_t>[]> written_{new std::atomic<uint64_t>[kPages]()};
};

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    if (!mant) {
      bits = sign;
    } else {
      float f = std::ldexp(float(mant), -24);
      return sign ? -f : f;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 112) << 23) | (mant << 13);
  }
  return std::bit_cast<float>(bits);
}

uint8_t ToUnorm8(float f) {
  if (!(f > 0.0f)) return 0;
  if (f >= 1.0f) return 255;
  return uint8_t(f * 255.0f + 0.5f);
}

// Formats are identified by their VkFormat (textures.cpp produces them); this
// is the Plume format for each.
pl::RenderFormat ToRenderFormat(VkFormat format) {
  using F = pl::RenderFormat;
  switch (format) {
    case VK_FORMAT_R8_UNORM: return F::R8_UNORM;
    case VK_FORMAT_R8_SNORM: return F::R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return F::R8G8_UNORM;
    case VK_FORMAT_R8G8_SNORM: return F::R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return F::R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM: return F::R8G8B8A8_SNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return F::R10G10B10A2_UNORM;
    case VK_FORMAT_R16_UNORM: return F::R16_UNORM;
    case VK_FORMAT_R16_SNORM: return F::R16_SNORM;
    case VK_FORMAT_R16_SFLOAT: return F::R16_FLOAT;
    case VK_FORMAT_R16G16_UNORM: return F::R16G16_UNORM;
    case VK_FORMAT_R16G16_SNORM: return F::R16G16_SNORM;
    case VK_FORMAT_R16G16_SFLOAT: return F::R16G16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM: return F::R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_SNORM: return F::R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return F::R16G16B16A16_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return F::R32_FLOAT;
    case VK_FORMAT_R32_UINT: return F::R32_UINT;
    case VK_FORMAT_R32G32_SFLOAT: return F::R32G32_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return F::R32G32B32A32_FLOAT;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return F::BC1_UNORM;
    case VK_FORMAT_BC2_UNORM_BLOCK: return F::BC2_UNORM;
    case VK_FORMAT_BC3_UNORM_BLOCK: return F::BC3_UNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return F::BC4_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK: return F::BC4_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: return F::BC5_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK: return F::BC5_SNORM;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return F::D32_FLOAT_S8_UINT;
    default: return F::UNKNOWN;
  }
}

bool IsBlockCompressed(VkFormat format) {
  return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
}

// Host format swizzle of the texture formats textures.cpp produces (the
// components a 1- or 2-channel format replicates, as in the SDK).
uint32_t HostFormatSwizzle(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
    default:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
}

// TextureCache::GuestToHostSwizzle (SDK, static member of the plugin).
uint32_t GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t c = (guest_swizzle >> (3 * i)) & 0b111;
    const uint32_t h = c >= xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? (c & 0b101)
                                                            : (host_format_swizzle >> (3 * c)) & 0b111;
    host_swizzle |= h << (3 * i);
  }
  return host_swizzle;
}

// The view of a texture's bits a shader's "signed" binding samples: the same
// data read as SNORM (formats without one are sampled as they are).
VkFormat SignedFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R16_UNORM: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_BC4_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_BC5_SNORM_BLOCK;
    default: return format;
  }
}

// Swizzle component (0-3 RGBA, 4 zero, 5 one).
pl::RenderSwizzle ComponentSwizzle(uint32_t c) {
  switch (c) {
    case 0: return pl::RenderSwizzle::R;
    case 1: return pl::RenderSwizzle::G;
    case 2: return pl::RenderSwizzle::B;
    case 3: return pl::RenderSwizzle::A;
    case 4: return pl::RenderSwizzle::ZERO;
    default: return pl::RenderSwizzle::ONE;
  }
}

VkFormat ColorTargetFormat(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:  // gamma applied in the shader (kSysFlag_ConvertColor*ToGamma)
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16:  // fixed -32...32: float keeps the range
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// Formats that are views of the same EDRAM data share a render target (the
// game draws its scene alternately as 2_10_10_10_FLOAT and
// 2_10_10_10_FLOAT_AS_16_16_16_16).
xenos::ColorRenderTargetFormat EdramFormat(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8_GAMMA: return F::k_8_8_8_8;
    case F::k_2_10_10_10_AS_10_10_10_10: return F::k_2_10_10_10;
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: return F::k_2_10_10_10_FLOAT;
    default: return format;
  }
}

// RGBA8 of one texel of a render target format (for the readback PNGs).
void TexelToRgba8(VkFormat format, const uint8_t* p, uint8_t out[4]) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
      std::memcpy(out, p, 4);
      return;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      uint32_t v;
      std::memcpy(&v, p, 4);
      out[0] = uint8_t((v & 0x3FF) >> 2), out[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
      out[2] = uint8_t(((v >> 20) & 0x3FF) >> 2), out[3] = uint8_t((v >> 30) * 85);
      return;
    }
    case VK_FORMAT_R8_UNORM:
      out[0] = out[1] = out[2] = p[0], out[3] = 255;
      return;
    case VK_FORMAT_R8G8_UNORM:
      out[0] = p[0], out[1] = p[1], out[2] = 0, out[3] = 255;
      return;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT: {
      const uint32_t n = format == VK_FORMAT_R16_SFLOAT ? 1 : format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
      out[2] = 0, out[3] = 255;
      for (uint32_t i = 0; i < n; ++i) {
        uint16_t h;
        std::memcpy(&h, p + i * 2, 2);
        out[i] = ToUnorm8(HalfToFloat(h));
      }
      if (n == 1) out[1] = out[2] = out[0];
      return;
    }
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
      const uint32_t n = format == VK_FORMAT_R32_SFLOAT ? 1 : format == VK_FORMAT_R32G32_SFLOAT ? 2 : 4;
      out[1] = out[2] = 0, out[3] = 255;
      for (uint32_t i = 0; i < n; ++i) {
        float f;
        std::memcpy(&f, p + i * 4, 4);
        out[i] = ToUnorm8(f);
      }
      if (n == 1) out[1] = out[2] = out[0];
      return;
    }
    default:
      out[0] = out[1] = out[2] = 255, out[3] = 255;
  }
}

// Bytes per texel of the formats TexelToRgba8 reads (0: not read back).
uint32_t FormatBytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16_SFLOAT:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    default:
      return 0;
  }
}

pl::RenderPrimitiveTopology ToRenderTopology(VkPrimitiveTopology topology) {
  using T = pl::RenderPrimitiveTopology;
  switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return T::POINT_LIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return T::LINE_LIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return T::LINE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return T::TRIANGLE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return T::TRIANGLE_FAN;
    default: return T::TRIANGLE_LIST;
  }
}

// --- Pipeline state ------------------------------------------------------------

struct PipelineKey {
  const void* vs;  // translations
  const void* ps;
  const void* layout;
  uint32_t topology;
  uint32_t cull_mode, front_face_clockwise, depth_clamp;
  uint32_t depth_test, depth_write, depth_compare;      // xenos::CompareFunction
  uint32_t stencil_test, stencil_front[4], stencil_back[4];  // fail, pass, depth fail, compare
  uint32_t color_count;
  uint32_t color_formats[4];  // VkFormat
  uint32_t blend[4][7];       // enable, src/dst color, op, src/dst alpha, op (Xenos values)
  uint32_t write_mask[4];
  uint32_t depth_format;  // VkFormat
  uint32_t samples;

  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

// xenos::BlendFactor -> Plume.
constexpr pl::RenderBlend kBlendFactors[32] = {
    pl::RenderBlend::ZERO,           // 0
    pl::RenderBlend::ONE,            // 1
    pl::RenderBlend::ZERO,           // 2 ?
    pl::RenderBlend::ZERO,           // 3 ?
    pl::RenderBlend::SRC_COLOR,      // 4
    pl::RenderBlend::INV_SRC_COLOR,  // 5
    pl::RenderBlend::SRC_ALPHA,      // 6
    pl::RenderBlend::INV_SRC_ALPHA,  // 7
    pl::RenderBlend::DEST_COLOR,     // 8
    pl::RenderBlend::INV_DEST_COLOR,  // 9
    pl::RenderBlend::DEST_ALPHA,      // 10
    pl::RenderBlend::INV_DEST_ALPHA,  // 11
    pl::RenderBlend::BLEND_FACTOR,    // 12 constant color
    pl::RenderBlend::INV_BLEND_FACTOR,
    pl::RenderBlend::BLEND_FACTOR_ALPHA,  // 14 constant alpha
    pl::RenderBlend::INV_BLEND_FACTOR_ALPHA,
    pl::RenderBlend::SRC_ALPHA_SAT,  // 16
    pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO,
    pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO,
    pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO,
    pl::RenderBlend::ZERO, pl::RenderBlend::ZERO, pl::RenderBlend::ZERO,
};
constexpr pl::RenderBlendOperation kBlendOps[8] = {
    pl::RenderBlendOperation::ADD, pl::RenderBlendOperation::SUBTRACT,
    pl::RenderBlendOperation::MIN, pl::RenderBlendOperation::MAX,
    pl::RenderBlendOperation::REV_SUBTRACT, pl::RenderBlendOperation::ADD,
    pl::RenderBlendOperation::ADD, pl::RenderBlendOperation::ADD};

// xenos::CompareFunction / StencilOp are Plume's enums minus their UNKNOWN.
pl::RenderComparisonFunction ToCompare(uint32_t f) { return pl::RenderComparisonFunction(f + 1); }
pl::RenderStencilOp ToStencilOp(uint32_t op) { return pl::RenderStencilOp(op + 1); }

// --- Resources -----------------------------------------------------------------

// A persistently mapped buffer allocated from front to back each frame.
struct Arena {
  std::unique_ptr<pl::RenderBuffer> buffer;
  uint8_t* mapping = nullptr;
  uint64_t size = 0, used = 0;
};

struct Image {
  std::unique_ptr<pl::RenderTexture> texture;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool is_3d = false, cube = false;
  uint32_t width = 1, height = 1, depth = 1, layers = 1, levels = 1;
  uint32_t samples = 1;
  bool mutable_format = false;  // viewed in other formats of the same size
  pl::RenderTextureLayout layout = pl::RenderTextureLayout::UNKNOWN;
  // (view dimension, swizzle, signed, view format) -> view
  std::map<uint64_t, std::unique_ptr<pl::RenderTextureView>> views;
  // Resolve destinations: the framebuffer resolves draw into.
  std::unique_ptr<pl::RenderFramebuffer> framebuffer;
};

struct RenderTarget {
  Image image;
  bool depth = false;
  uint32_t rows = 0;  // guest rows the image holds (its height / resolution scale)
  uint32_t base = 0, format = 0, pitch = 0, samples = 1;  // its key (diagnostics)
  uint64_t written_frame = 0;  // last frame a pass drew into or cleared it
};

struct RenderTargetKey {
  uint32_t base, format, pitch, depth, samples;
  bool operator<(const RenderTargetKey& o) const {
    return std::tie(base, format, pitch, depth, samples) <
           std::tie(o.base, o.format, o.pitch, o.depth, o.samples);
  }
};

struct TextureEntry {
  Image image;
  uint64_t content_hash = 0;
  uint64_t validated_frame = 0;
  uint64_t watched_epoch = 0;  // PageWatch epoch the content was hashed at
  // Depth resolves: image holds float depth (for depth fetch formats), packed
  // the 32-bit words the console writes to memory, (depth24 << 8) | stencil,
  // for textures that read that memory as color (the composite pass reads the
  // scene depth as 8_8_8_8 and blends its color grading by the stencil byte).
  bool depth = false;
  Image packed;
  bool packed_wanted = false;  // sampled as color: packed from the next resolve on
  // Resolved textures hold the render target's components in its order; in
  // guest memory a resolve stores them so that sampling through the
  // destination's fetch swizzle gives that order back.
  bool from_resolve = false;
  uint32_t resolve_swizzle = 0;  // the destination fetch constant's swizzle
  // Resolved textures: the guest memory they stand for, as it was when
  // resolved (nothing writes it: the CPU replacing it means a texture again).
  uint32_t memory_address = 0, memory_bytes = 0;
  uint64_t memory_sample = 0;
};

// A cheap fingerprint of a guest memory range: 16 spread 64-byte pieces.
uint64_t SampleMemory(const uint8_t* memory, uint32_t address, uint32_t bytes) {
  if (!bytes || uint64_t(address) + bytes > 0x20000000) return 0;
  const uint8_t* base = memory + 0xA0000000u + address;
  uint64_t hash = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t offset = uint32_t(uint64_t(bytes) * i / 16);
    hash = XXH3_64bits_withSeed(base + offset, std::min<uint32_t>(64, bytes - offset), hash);
  }
  return hash;
}

// View swizzle for sampling a resolved texture through a fetch swizzle:
// memory component m holds render target component k where
// resolve_swizzle[k] == m, and the image holds k at k.
uint32_t ResolvedTextureSwizzle(uint32_t fetch_swizzle, uint32_t resolve_swizzle) {
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t m = (fetch_swizzle >> (3 * c)) & 7;
    uint32_t h = m >= 4 ? (m & 0b101) : 4;  // constants pass, unknown -> 0
    if (m < 4)
      for (uint32_t k = 0; k < 4; ++k)
        if (((resolve_swizzle >> (3 * k)) & 7) == m) {
          h = k;
          break;
        }
    out |= h << (3 * c);
  }
  return out;
}

struct Readback {
  ResolvedImage result;
  std::unique_ptr<pl::RenderBuffer> buffer;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

// Non-finite components in texels of a float format (0 for other formats).
uint32_t CountNonFinite(VkFormat format, const uint8_t* p, size_t texels) {
  uint32_t bad = 0;
  if (format == VK_FORMAT_R16_SFLOAT || format == VK_FORMAT_R16G16_SFLOAT ||
      format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    const size_t n = texels * (format == VK_FORMAT_R16_SFLOAT ? 1 : format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4);
    for (size_t i = 0; i < n; ++i) {
      uint16_t h;
      std::memcpy(&h, p + i * 2, 2);
      bad += (h & 0x7C00) == 0x7C00;
    }
  } else if (format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT ||
             format == VK_FORMAT_R32G32B32A32_SFLOAT) {
    const size_t n = texels * (format == VK_FORMAT_R32_SFLOAT ? 1 : format == VK_FORMAT_R32G32_SFLOAT ? 2 : 4);
    for (size_t i = 0; i < n; ++i) {
      float f;
      std::memcpy(&f, p + i * 4, 4);
      bad += !std::isfinite(f);
    }
  }
  return bad;
}

constexpr uint32_t kFramesInFlight = 2;

// Descriptor sets of one layout, per frame in flight, reused frame after frame
// (each draw takes the next one; that frame has finished before they are
// rewritten).
struct SetPool {
  pl::RenderDescriptorSetBuilder builder;
  std::vector<std::unique_ptr<pl::RenderDescriptorSet>> sets[kFramesInFlight];
  size_t used[kFramesInFlight] = {};

  pl::RenderDescriptorSet* Next(pl::RenderDevice* device, uint32_t frame) {
    if (used[frame] == sets[frame].size()) sets[frame].push_back(builder.create(device));
    return sets[frame][used[frame]++].get();
  }
  void Clear() {
    for (auto& s : sets) s.clear();
  }
};

enum ViewKind : uint32_t { kView2DArray, kView3D, kViewCube, kViewStencil };

}  // namespace

// --- Implementation --------------------------------------------------------

struct Renderer::Impl {
  const VulkanDevice* dev = nullptr;
  std::unique_ptr<PlumeDevice> plume;
  pl::RenderDevice* device = nullptr;
  std::unique_ptr<ShaderLibrary> library;

  std::unique_ptr<pl::RenderCommandQueue> queue;
  // What each frame in flight owns.
  struct FrameContext {
    // Texture uploads go into their own list, submitted before the draws.
    std::unique_ptr<pl::RenderCommandList> upload_list, draw_list;
    std::unique_ptr<pl::RenderCommandFence> fence;
    bool submitted = false;
    Arena vertex_arena, uniform_arena, staging_arena;
    pl::RenderDescriptorSet* shared_set = nullptr;  // the vertex arena
    std::vector<Image> garbage;  // replaced during the frame, destroyed after it
    // The presenter's guest output as the frame wrote it.
    std::vector<std::unique_ptr<pl::RenderFramebuffer>> output_framebuffers;
    std::vector<std::unique_ptr<pl::RenderTexture>> outputs;
    // Start and end of the frame's commands, then one after each resolve.
    std::unique_ptr<pl::RenderQueryPool> timestamps;
    uint32_t resolve_stamps = 0;
    std::vector<std::unique_ptr<pl::RenderFramebuffer>> garbage_framebuffers;
    // Debug (SetDebugFrames): a patch of the frame's front buffer, read back
    // without waiting, and the frame's draw log.
    std::unique_ptr<pl::RenderBuffer> probe;
    bool probed = false;
    uint64_t probe_frame = 0;
    std::vector<std::string> log;
    // Small float resolves (the luminance chain), checked for non-finite values
    // once the frame is done: an inf / NaN there blacks out the whole scene.
    std::vector<Readback> checks;
    uint64_t checks_frame = 0;
    // Black-frame detector: 16 patches of the presented frame, and that
    // frame's resolves (for the log).
    std::unique_ptr<pl::RenderBuffer> dark_probe;
    bool dark_probed = false;
    uint64_t dark_frame = 0;
    std::string dark_resolves;
    uint32_t dark_draws = 0;
    // An 8x8 patch from the middle of each larger color resolve, read back
    // every frame: which stage of the frame went black.
    std::unique_ptr<pl::RenderBuffer> stage_probe;
    struct StageProbe {
      uint32_t index, dest_base, width, height;
      VkFormat format;
    };
    std::vector<StageProbe> stage_probes, stage_probes_done;
  };
  static constexpr uint32_t kStageProbes = 32, kStageProbeBytes = 8 * 8 * 8;
  FrameContext contexts[kFramesInFlight];
  uint32_t context_index = 0;
  FrameContext& Context() { return contexts[context_index]; }
  // The current frame's (BeginFrame).
  pl::RenderCommandList* upload_list = nullptr;
  pl::RenderCommandList* draw_list = nullptr;
  Arena* vertex_arena = nullptr;
  Arena* uniform_arena = nullptr;
  Arena* staging_arena = nullptr;
  double timestamp_period_ns = 0;  // 0: no GPU timing

  // Descriptor set layouts (VulkanCommandProcessor::SetupContext): shared
  // memory (the vertex arena), constants, textures + samplers per stage.
  SetPool shared_pool, constants_pool, empty_pool;
  std::map<std::tuple<bool, uint32_t, uint32_t>, SetPool> texture_pools;
  pl::RenderDescriptorSet* shared_set = nullptr;  // the current frame's
  std::map<std::array<uint32_t, 4>, std::unique_ptr<pl::RenderPipelineLayout>> pipeline_layouts;
  std::unordered_map<const void*, std::unique_ptr<pl::RenderShader>> shaders;
  std::unordered_map<PipelineKey, std::unique_ptr<pl::RenderPipeline>, PipelineKeyHash> pipelines;
  std::map<uint64_t, std::unique_ptr<pl::RenderSampler>> samplers;
  std::map<RenderTargetKey, RenderTarget> render_targets;
  std::map<std::array<const void*, 5>, std::unique_ptr<pl::RenderFramebuffer>> framebuffers;
  std::unordered_map<uint64_t, TextureEntry> textures;  // key hash -> texture
  std::unordered_map<uint32_t, TextureEntry> resolved;   // this frame: dest base page -> texture
  Image dummy_2d, dummy_cube, dummy_3d;
  std::unordered_map<uint32_t, bool> linear_filterable;  // VkFormat ->

  // Resolves: a fullscreen triangle sampling the render target.
  std::unique_ptr<pl::RenderShader> resolve_vs_shader, resolve_shader, resolve_ms_shader;
  std::unique_ptr<pl::RenderSampler> point_sampler;
  SetPool resolve_pool;
  std::unique_ptr<pl::RenderPipelineLayout> resolve_layout;
  std::map<std::tuple<uint32_t, uint32_t, bool>, std::unique_ptr<pl::RenderPipeline>>
      resolve_pipelines;  // (dest format, samples, depth)
  // Present pass (glsl/present_gamma_table.frag), FXAA (glsl/present_fxaa.frag).
  std::unique_ptr<pl::RenderShader> present_shader, fxaa_shader;
  SetPool present_pool, fxaa_pool;
  std::unique_ptr<pl::RenderPipelineLayout> present_layout, fxaa_layout;
  std::unique_ptr<pl::RenderPipeline> present_pipeline, fxaa_pipeline;
  std::unique_ptr<pl::RenderSampler> linear_sampler;
  bool fxaa = false;
  Image fxaa_image;  // the gamma-corrected frame FXAA reads
  // MSAA: sample counts usable for color, depth and stencil (bit n = n samples),
  // and the count for the game's MSAA targets (0 = the game's).
  uint32_t msaa_supported = 1;
  uint32_t msaa_override = 0;
  // Packed depth resolves (glsl/pack_depth*.frag).
  std::unique_ptr<pl::RenderShader> pack_shader, pack_ms_shader;
  SetPool pack_pool;
  std::unique_ptr<pl::RenderPipelineLayout> pack_layout;
  std::map<uint32_t, std::unique_ptr<pl::RenderPipeline>> pack_pipelines;  // samples ->

  // Vertex buffers copied this frame: (address, size) -> offset + watch epoch;
  // or with no page watch, (address, size, content hash) -> offset.
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> vertex_copies;
  uint64_t uniform_ranges[SpirvShaderTranslator::kConstantBufferCount] = {};

  const uint8_t* memory = nullptr;
  uint64_t frame = 0;
  bool readback = false;       // this frame's resolves are read back
  bool keep_resolves = false;  // SetKeepResolves
  int32_t anisotropic_override = -1;  // the SDK's setting, read per frame
  bool frame_open = false;
  bool rendering = false;
  std::array<RenderTarget*, 4> pass_colors{};
  RenderTarget* pass_depth = nullptr;
  std::vector<Readback> readbacks;
  RendererStats stats;

  ~Impl();
  uint32_t scale = 1;  // resolution scale: host pixels per guest pixel, both axes
  uint32_t skip_first = 0, skip_end = 0;  // debug: draws [first, end) are not drawn
  // Debug: per-draw log, dumped for frames whose front buffer patch is much
  // brighter than usual (with the last normal frame's, to diff them).
  std::string debug_dir;
  std::vector<std::string> frame_log, last_normal_log;
  double probe_average = -1;
  double dark_average = -1;   // brightness of the last presented frame
  std::string frame_resolves;  // this frame's resolves (black-frame log)
  std::string previous_small_values;  // the last checked frame's small resolves
  std::string previous_stage_values;  // and its resolve patches
  std::vector<std::string> previous_frame_log;  // debug_dir: the last checked frame's draw log
  uint32_t dark_dumps = 0;
  uint64_t memory_dump_frame = 0, memory_dump_name = 0;  // debug_dir: dump after a black frame
  std::vector<FrameContext::StageProbe> memory_dump_targets;
  std::unordered_set<uint64_t> dumped_shaders;  // debug_dir
  uint64_t probe_bytes = 0;
  uint32_t probe_dumps = 0;
  bool Init(const VulkanDevice* device, std::string* error);
  void Note(std::string note) {
    if (stats.notes.size() < 40) stats.notes.push_back(std::move(note));
  }

  bool CreateArena(Arena& arena, uint64_t size, pl::RenderBufferFlags flags);
  // Allocates from a per-frame arena; nullptr if full.
  uint8_t* Allocate(Arena& arena, uint64_t size, uint64_t alignment, uint64_t& offset_out);

  // flags: RENDER_TARGET / DEPTH_TARGET (plus CUBE, MUTABLE_FORMAT from the image).
  bool CreateImage(Image& image, pl::RenderTextureFlags flags);
  // For images the frame's commands may still use.
  void DestroyLater(Image& image) {
    if (image.texture) Context().garbage.push_back(std::move(image));
    image = Image{};
  }
  bool LinearFilterable(VkFormat format);
  // view_format: another format of the same size (mutable-format images).
  pl::RenderTextureView* View(Image& image, ViewKind kind, uint32_t swizzle, bool is_signed = false,
                              VkFormat view_format = VK_FORMAT_UNDEFINED);
  // always: a barrier even when the layout stays - render passes writing the
  // same attachment one after another need one (Plume's render passes carry
  // no dependencies; RADV runs unsynchronized passes concurrently).
  void Transition(pl::RenderCommandList& list, Image& image, pl::RenderTextureLayout layout,
                  bool always = false);

  // rows: guest rows the caller needs (0 = what it has); grows the target.
  RenderTarget* GetRenderTarget(const RegisterFile& regs, int color_index /* -1 = depth */,
                                uint32_t rows = 0);
  bool GrowRenderTarget(RenderTarget& rt, uint32_t rows);
  pl::RenderFramebuffer* Framebuffer(const std::array<RenderTarget*, 4>& colors, uint32_t count,
                                     RenderTarget* depth);
  bool BeginRendering(const std::array<RenderTarget*, 4>& colors, RenderTarget* depth);
  void EndRendering();
  // Clears a render target, or a rectangle of it.
  void ClearRegion(RenderTarget& rt, const pl::RenderRect* rect, const float color[4], float depth,
                   uint32_t stencil, bool clear_depth, bool clear_stencil);

  TextureEntry* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  // The texture and view a binding samples.
  // scaled_out: the texture is resolution-scaled (a resolve's).
  std::pair<pl::RenderTexture*, pl::RenderTextureView*> TextureView(
      const RegisterFile& regs, uint32_t fetch_index, xenos::FetchOpDimension dimension,
      bool is_signed, bool* scaled_out);
  pl::RenderSampler* Sampler(const RegisterFile& regs, const SpirvShader::SamplerBinding& binding);

  SetPool& TexturePool(bool vertex, uint32_t textures, uint32_t samplers);
  pl::RenderPipelineLayout* PipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp, uint32_t sp);
  pl::RenderShader* Shader(const SpirvShader::Translation& translation);
  pl::RenderPipeline* Pipeline(const PipelineKey& key);
  pl::RenderPipeline* ResolvePipeline(VkFormat dest_format, uint32_t samples, bool depth);
  pl::RenderPipeline* PackPipeline(uint32_t samples);
  void RecordPresent(VkImage target, bool target_read_before, TextureEntry& source,
                     const uint32_t* gamma_ramp);
  // A resolve's draw: the render target region into a destination texture.
  void DrawResolve(Image& dest, RenderTarget& rt, pl::RenderPipeline* pipeline,
                   pl::RenderPipelineLayout* layout, pl::RenderDescriptorSet* set,
                   const pl::RenderRect& scissor, const void* constants, uint32_t constants_size);

  void Draw(const RegisterFile& regs, std::span<const uint32_t> vs_ucode,
            std::span<const uint32_t> ps_ucode, const GuestDraw& draw);
};

Renderer::Impl::~Impl() {
  if (!device) return;
  // Destroy everything Plume made (after the frames in flight) before the
  // device wrapper goes.
  for (FrameContext& c : contexts)
    if (c.submitted) queue->waitForCommandFence(c.fence.get());
  framebuffers.clear();
  render_targets.clear();
  textures.clear();
  resolved.clear();
  dummy_2d = Image{}, dummy_cube = Image{}, dummy_3d = Image{};
  readbacks.clear();
  pipelines.clear();
  resolve_pipelines.clear();
  pipeline_layouts.clear();
  resolve_layout.reset();
  shaders.clear();
  pack_pipelines.clear();
  pack_layout.reset();
  pack_pool.Clear();
  fxaa_image = Image{};
  present_pipeline.reset(), fxaa_pipeline.reset();
  present_layout.reset(), fxaa_layout.reset();
  present_pool.Clear(), fxaa_pool.Clear();
  present_shader.reset(), fxaa_shader.reset();
  linear_sampler.reset();
  pack_shader.reset(), pack_ms_shader.reset();
  resolve_vs_shader.reset(), resolve_shader.reset(), resolve_ms_shader.reset();
  samplers.clear();
  point_sampler.reset();
  texture_pools.clear();
  shared_pool.Clear(), constants_pool.Clear(), empty_pool.Clear();
  resolve_pool.Clear();
  for (FrameContext& c : contexts) {
    for (Arena* a : {&c.vertex_arena, &c.uniform_arena, &c.staging_arena})
      if (a->buffer) a->buffer->unmap();
    c = FrameContext{};
  }
  queue.reset();
}

bool Renderer::Impl::CreateArena(Arena& arena, uint64_t size, pl::RenderBufferFlags flags) {
  arena.buffer = device->createBuffer(pl::RenderBufferDesc::UploadBuffer(size, flags));
  if (!arena.buffer) return false;
  arena.mapping = static_cast<uint8_t*>(arena.buffer->map());
  arena.size = size;
  return arena.mapping != nullptr;
}

uint8_t* Renderer::Impl::Allocate(Arena& arena, uint64_t size, uint64_t alignment,
                                  uint64_t& offset_out) {
  const uint64_t offset = (arena.used + alignment - 1) / alignment * alignment;
  if (offset + size > arena.size) return nullptr;
  arena.used = offset + size;
  offset_out = offset;
  return arena.mapping + offset;
}

bool Renderer::Impl::CreateImage(Image& image, pl::RenderTextureFlags flags) {
  const pl::RenderFormat format = ToRenderFormat(image.format);
  if (format == pl::RenderFormat::UNKNOWN) return false;
  if (image.cube) flags |= pl::RenderTextureFlag::CUBE;
  if (image.mutable_format || SignedFormat(image.format) != image.format)
    flags |= pl::RenderTextureFlag::MUTABLE_FORMAT;
  pl::RenderTextureDesc desc = pl::RenderTextureDesc::Texture(
      image.is_3d ? pl::RenderTextureDimension::TEXTURE_3D : pl::RenderTextureDimension::TEXTURE_2D,
      image.width, image.height, image.depth, image.levels, image.layers, format, flags);
  desc.multisampling.sampleCount = image.samples;
  image.texture = device->createTexture(desc);
  image.layout = pl::RenderTextureLayout::UNKNOWN;
  return image.texture != nullptr;
}

pl::RenderTextureView* Renderer::Impl::View(Image& image, ViewKind kind, uint32_t swizzle,
                                            bool is_signed, VkFormat view_format) {
  auto [it, inserted] = image.views.try_emplace(uint64_t(view_format) << 32 | uint32_t(kind) << 16 |
                                                uint32_t(is_signed) << 12 | swizzle);
  if (!inserted) return it->second.get();
  pl::RenderTextureViewDesc desc;
  const VkFormat format = view_format != VK_FORMAT_UNDEFINED ? view_format : image.format;
  desc.format = ToRenderFormat(is_signed ? SignedFormat(format) : format);
  desc.stencilAspect = kind == kViewStencil;
  desc.dimension = kind == kView3D     ? pl::RenderTextureViewDimension::TEXTURE_3D
                   : kind == kViewCube ? pl::RenderTextureViewDimension::TEXTURE_CUBE
                                       : pl::RenderTextureViewDimension::TEXTURE_2D;
  desc.forceArray = kind == kView2DArray;  // the translated shaders sample 2D as arrays
  desc.componentMapping = pl::RenderComponentMapping(
      ComponentSwizzle(swizzle & 7), ComponentSwizzle((swizzle >> 3) & 7),
      ComponentSwizzle((swizzle >> 6) & 7), ComponentSwizzle((swizzle >> 9) & 7));
  it->second = image.texture->createTextureView(desc);
  return it->second.get();
}

void Renderer::Impl::Transition(pl::RenderCommandList& list, Image& image,
                                pl::RenderTextureLayout layout, bool always) {
  if (image.layout == layout && !always) return;
  list.barriers(kAllStages, pl::RenderTextureBarrier(image.texture.get(), layout));
  image.layout = layout;
}

bool Renderer::Impl::Init(const VulkanDevice* sdk_device, std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  dev = sdk_device;
  plume = CreatePlumeDevice(sdk_device, error);
  if (!plume) return false;
  device = plume->device.get();
  library = std::make_unique<ShaderLibrary>(SpirvShaderTranslator::Features(dev), scale);

  queue = device->createCommandQueue(pl::RenderCommandListType::DIRECT);
  if (!queue) return fail("Plume: no command queue");
  shared_pool.builder.begin();
  shared_pool.builder.addStructuredBuffer(0);
  shared_pool.builder.end();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    FrameContext& c = contexts[i];
    c.upload_list = queue->createCommandList();
    c.draw_list = queue->createCommandList();
    c.fence = device->createCommandFence();
    if (!c.upload_list || !c.draw_list || !c.fence) return fail("Plume: no command lists");
    if (!CreateArena(c.vertex_arena, kVertexArenaSize,
                     pl::RenderBufferFlag::STORAGE | pl::RenderBufferFlag::INDEX) ||
        !CreateArena(c.uniform_arena, kUniformArenaSize, pl::RenderBufferFlag::CONSTANT) ||
        !CreateArena(c.staging_arena, kStagingArenaSize, pl::RenderBufferFlag::NONE))
      return fail("buffer allocation failed");
    c.shared_set = shared_pool.Next(device, i);
    c.shared_set->setBuffer(0, c.vertex_arena.buffer.get());
    c.timestamps = device->createQueryPool(2 + kResolveStamps);
  }
  {
    VkPhysicalDeviceProperties properties;
    dev->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(dev->physical_device(),
                                                                      &properties);
    if (contexts[0].timestamps) timestamp_period_ns = properties.limits.timestampPeriod;
  }
  constants_pool.builder.begin();
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i)
    constants_pool.builder.addConstantBuffer(i);
  constants_pool.builder.end();
  empty_pool.builder.begin();
  empty_pool.builder.end();

  // Shader constant ranges (float constants: up to 256 vec4 per stage).
  uniform_ranges[SpirvShaderTranslator::kConstantBufferSystem] =
      sizeof(SpirvShaderTranslator::SystemConstants);
  uniform_ranges[SpirvShaderTranslator::kConstantBufferFloatVertex] = 256 * 16;
  uniform_ranges[SpirvShaderTranslator::kConstantBufferFloatPixel] = 256 * 16;
  uniform_ranges[SpirvShaderTranslator::kConstantBufferBoolLoop] = (8 + 32) * 4;
  uniform_ranges[SpirvShaderTranslator::kConstantBufferFetch] = 32 * 6 * 4;

  // Placeholders for textures that are missing or can't be converted (zeroed
  // in the first frame).
  auto dummy = [&](Image& image, bool is_3d, uint32_t layers, bool cube) {
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.is_3d = is_3d;
    image.layers = layers;
    image.cube = cube;
    return CreateImage(image, pl::RenderTextureFlag::NONE);
  };
  if (!dummy(dummy_2d, false, 1, false) || !dummy(dummy_cube, false, 6, true) ||
      !dummy(dummy_3d, true, 1, false))
    return fail("placeholder textures");

  // Resolves.
  resolve_vs_shader = device->createShader(present_vs, sizeof(present_vs), "main",
                                           pl::RenderShaderFormat::SPIRV);
  resolve_shader = device->createShader(resolve_ps, sizeof(resolve_ps), "main",
                                        pl::RenderShaderFormat::SPIRV);
  resolve_ms_shader = device->createShader(resolve_ms_ps, sizeof(resolve_ms_ps), "main",
                                           pl::RenderShaderFormat::SPIRV);
  pl::RenderSamplerDesc point;
  point.minFilter = point.magFilter = pl::RenderFilter::NEAREST;
  point.mipmapMode = pl::RenderMipmapMode::NEAREST;
  point.addressU = point.addressV = point.addressW = pl::RenderTextureAddressMode::CLAMP;
  point_sampler = device->createSampler(point);
  resolve_pool.builder.begin();
  resolve_pool.builder.addTexture(0);
  resolve_pool.builder.addSampler(1);
  resolve_pool.builder.end();
  pl::RenderPipelineLayoutBuilder layout;
  layout.begin();
  layout.addPushConstant(0, 0, 8, pl::RenderShaderStageFlag::PIXEL);
  layout.addDescriptorSet(resolve_pool.builder);
  layout.end();
  resolve_layout = layout.create(device);
  // Presentation.
  present_shader = device->createShader(present_gamma_table_ps, sizeof(present_gamma_table_ps),
                                        "main", pl::RenderShaderFormat::SPIRV);
  present_pool.builder.begin();
  present_pool.builder.addTexture(0);
  present_pool.builder.addConstantBuffer(1);
  present_pool.builder.addSampler(2);
  present_pool.builder.end();
  layout.begin();
  layout.addDescriptorSet(present_pool.builder);
  layout.end();
  present_layout = layout.create(device);
  if (present_shader && present_layout) {
    pl::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = present_layout.get();
    desc.vertexShader = resolve_vs_shader.get();
    desc.pixelShader = present_shader.get();
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = pl::RenderFormat::R10G10B10A2_UNORM;  // kGuestOutputFormat
    desc.renderTargetBlend[0] = pl::RenderBlendDesc::Copy();
    present_pipeline = device->createGraphicsPipeline(desc);
  }
  fxaa_shader = device->createShader(present_fxaa_ps, sizeof(present_fxaa_ps), "main",
                                     pl::RenderShaderFormat::SPIRV);
  fxaa_pool.builder.begin();
  fxaa_pool.builder.addTexture(0);
  fxaa_pool.builder.addSampler(1);
  fxaa_pool.builder.end();
  layout.begin();
  layout.addDescriptorSet(fxaa_pool.builder);
  layout.end();
  fxaa_layout = layout.create(device);
  pl::RenderSamplerDesc linear;
  linear.mipmapMode = pl::RenderMipmapMode::NEAREST;
  linear.addressU = linear.addressV = linear.addressW = pl::RenderTextureAddressMode::CLAMP;
  linear_sampler = device->createSampler(linear);
  if (fxaa_shader && fxaa_layout) {
    pl::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = fxaa_layout.get();
    desc.vertexShader = resolve_vs_shader.get();
    desc.pixelShader = fxaa_shader.get();
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = pl::RenderFormat::R10G10B10A2_UNORM;
    desc.renderTargetBlend[0] = pl::RenderBlendDesc::Copy();
    fxaa_pipeline = device->createGraphicsPipeline(desc);
  }
  {
    const auto& props = dev->properties();
    msaa_supported = props.framebufferColorSampleCounts & props.framebufferDepthSampleCounts &
                     props.framebufferStencilSampleCounts;
  }

  pack_shader = device->createShader(pack_depth_ps, sizeof(pack_depth_ps), "main",
                                     pl::RenderShaderFormat::SPIRV);
  pack_ms_shader = device->createShader(pack_depth_ms_ps, sizeof(pack_depth_ms_ps), "main",
                                        pl::RenderShaderFormat::SPIRV);
  pack_pool.builder.begin();
  pack_pool.builder.addTexture(0);
  pack_pool.builder.addTexture(1);
  pack_pool.builder.addSampler(2);
  pack_pool.builder.end();
  layout.begin();
  layout.addPushConstant(0, 0, 12, pl::RenderShaderStageFlag::PIXEL);
  layout.addDescriptorSet(pack_pool.builder);
  layout.end();
  pack_layout = layout.create(device);
  if (!resolve_vs_shader || !resolve_shader || !resolve_ms_shader || !point_sampler ||
      !resolve_layout || !pack_shader || !pack_ms_shader || !pack_layout || !present_pipeline ||
      !fxaa_pipeline || !linear_sampler)
    return fail("resolve pipeline objects");
  return true;
}

// --- Render targets --------------------------------------------------------

RenderTarget* Renderer::Impl::GetRenderTarget(const RegisterFile& regs, int color_index,
                                              uint32_t rows) {
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  RenderTargetKey key;
  VkFormat format;
  if (color_index < 0) {
    const auto info = regs.Get<reg::RB_DEPTH_INFO>();
    key = {info.depth_base, uint32_t(info.depth_format), surface.surface_pitch, 1, 0};
    format = kDepthFormat;
  } else {
    const auto info = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[color_index]);
    key = {info.color_base, uint32_t(EdramFormat(info.color_format)), surface.surface_pitch, 0, 0};
    format = ColorTargetFormat(info.color_format);
  }
  if (format == VK_FORMAT_UNDEFINED || !surface.surface_pitch) return nullptr;
  key.samples = 1u << std::min(uint32_t(surface.msaa_samples), 2u);  // 1x, 2x, 4x
  if (key.samples > 1 && msaa_override) {
    // The game's MSAA targets (its scene) at another sample count: the largest
    // the device supports up to the setting.
    key.samples = 1;
    for (uint32_t n = 2; n <= msaa_override; n <<= 1)
      if (msaa_supported & n) key.samples = n;
  }
  auto [it, inserted] = render_targets.try_emplace(key);
  RenderTarget& rt = it->second;
  if (inserted) {
    rt.base = key.base, rt.format = key.format, rt.pitch = key.pitch, rt.samples = key.samples;
    rt.depth = color_index < 0;
    rt.image.format = format;
    rt.image.width = surface.surface_pitch * scale;
    rt.image.samples = key.samples;
  } else if (!rt.image.texture && !rt.rows) {
    return nullptr;  // creation failed before
  }
  if (!rt.image.texture || rows > rt.rows) {
    if (!GrowRenderTarget(rt, std::max<uint32_t>(rows, 720))) {
      Note(fmt::format("render target {:X} format {:X} pitch {}: texture creation failed", key.base,
                       key.format, key.pitch));
      return nullptr;
    }
  }
  return &rt;
}

// (Re)creates the target's texture with at least this many guest rows,
// keeping what the old one held.
bool Renderer::Impl::GrowRenderTarget(RenderTarget& rt, uint32_t rows) {
  rows = std::min((rows + kRenderTargetRowsStep - 1) / kRenderTargetRowsStep * kRenderTargetRowsStep,
                  kRenderTargetRowsMax);
  if (rt.image.texture && rows <= rt.rows) return true;
  EndRendering();  // clears and copies below happen outside rendering
  Image old = std::move(rt.image);
  rt.image = Image{};
  rt.image.format = old.format;
  rt.image.width = old.width;
  rt.image.samples = old.samples;
  rt.image.height = rows * scale;
  if (!CreateImage(rt.image, rt.depth ? pl::RenderTextureFlag::DEPTH_TARGET
                                      : pl::RenderTextureFlag::RENDER_TARGET)) {
    rt.image = std::move(old);
    return rt.image.texture && rt.rows >= rows;
  }
  // Framebuffers of the old texture go after the frames using them.
  for (auto it = framebuffers.begin(); it != framebuffers.end();) {
    if (std::find(it->first.begin(), it->first.end(), &rt) != it->first.end()) {
      Context().garbage_framebuffers.push_back(std::move(it->second));
      it = framebuffers.erase(it);
    } else {
      ++it;
    }
  }
  // Start cleared (EDRAM contents before the first clear are undefined anyway),
  // then the old contents on top.
  const float black[4] = {};
  ClearRegion(rt, nullptr, black, 1.0f, 0, true, true);
  if (old.texture) {
    Transition(*draw_list, old, pl::RenderTextureLayout::COPY_SOURCE);
    Transition(*draw_list, rt.image, pl::RenderTextureLayout::COPY_DEST);
    const pl::RenderBox box(0, 0, int32_t(old.width), int32_t(old.height));
    draw_list->copyTextureRegion(pl::RenderTextureCopyLocation::Subresource(rt.image.texture.get()),
                                 pl::RenderTextureCopyLocation::Subresource(old.texture.get()), 0, 0,
                                 0, &box);
    DestroyLater(old);
  }
  if (rt.rows) Note(fmt::format("render target grown from {} to {} rows", rt.rows, rows));
  rt.rows = rows;
  return true;
}

pl::RenderFramebuffer* Renderer::Impl::Framebuffer(const std::array<RenderTarget*, 4>& colors,
                                                   uint32_t count, RenderTarget* depth) {
  std::array<const void*, 5> key{};
  for (uint32_t i = 0; i < count; ++i) key[i] = colors[i];
  key[4] = depth;
  auto [it, inserted] = framebuffers.try_emplace(key);
  if (!inserted) return it->second.get();
  const pl::RenderTexture* attachments[4] = {};
  for (uint32_t i = 0; i < count; ++i) attachments[i] = colors[i]->image.texture.get();
  it->second = device->createFramebuffer(pl::RenderFramebufferDesc(
      attachments, count, depth ? depth->image.texture.get() : nullptr));
  return it->second.get();
}

void Renderer::Impl::EndRendering() {
  if (!rendering) return;
  draw_list->setFramebuffer(nullptr);
  rendering = false;
}

bool Renderer::Impl::BeginRendering(const std::array<RenderTarget*, 4>& colors,
                                    RenderTarget* depth) {
  if (rendering && colors == pass_colors && depth == pass_depth) return true;
  EndRendering();
  uint32_t count = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (colors[i]) count = i + 1;
  // Plume framebuffers have no holes between color attachments.
  for (uint32_t i = 0; i < count; ++i)
    if (!colors[i]) return false;
  if (!count && !depth) return false;
  // One barrier for all attachments, also when they are already in the layout:
  // the previous pass on them may still be writing.
  std::vector<pl::RenderTextureBarrier> barriers;
  for (uint32_t i = 0; i < count; ++i) {
    barriers.emplace_back(colors[i]->image.texture.get(), pl::RenderTextureLayout::COLOR_WRITE);
    colors[i]->image.layout = pl::RenderTextureLayout::COLOR_WRITE;
  }
  if (depth) {
    barriers.emplace_back(depth->image.texture.get(), pl::RenderTextureLayout::DEPTH_WRITE);
    depth->image.layout = pl::RenderTextureLayout::DEPTH_WRITE;
  }
  draw_list->barriers(kAllStages, barriers);
  for (uint32_t i = 0; i < count; ++i) colors[i]->written_frame = frame;
  if (depth) depth->written_frame = frame;
  pl::RenderFramebuffer* framebuffer = Framebuffer(colors, count, depth);
  if (!framebuffer) return false;
  draw_list->setFramebuffer(framebuffer);
  rendering = true;
  pass_colors = colors;
  pass_depth = depth;
  ++stats.render_passes;
  return true;
}

void Renderer::Impl::ClearRegion(RenderTarget& rt, const pl::RenderRect* rect,
                                 const float color[4], float depth, uint32_t stencil,
                                 bool clear_depth, bool clear_stencil) {
  pl::RenderRect clamped;
  if (rect) {
    clamped = *rect;
    clamped.right = std::min(clamped.right, int32_t(rt.image.width));
    clamped.bottom = std::min(clamped.bottom, int32_t(rt.image.height));
    if (clamped.right <= clamped.left || clamped.bottom <= clamped.top) return;
  }
  std::array<RenderTarget*, 4> colors{};
  if (!rt.depth) colors[0] = &rt;
  if (!BeginRendering(colors, rt.depth ? &rt : nullptr)) return;
  if (rt.depth)
    draw_list->clearDepthStencil(clear_depth, clear_stencil, depth, stencil,
                                 rect ? &clamped : nullptr, rect ? 1 : 0);
  else
    draw_list->clearColor(0, pl::RenderColor(color[0], color[1], color[2], color[3]),
                          rect ? &clamped : nullptr, rect ? 1 : 0);
  EndRendering();
}

// --- Textures and samplers -------------------------------------------------

TextureEntry* Renderer::Impl::GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (fetch.type != xenos::FetchConstantType::kTexture) return nullptr;
  if (auto it = resolved.find(fetch.base_address); it != resolved.end()) {
    TextureEntry& r = it->second;
    if (keep_resolves || SampleMemory(memory, r.memory_address, r.memory_bytes) == r.memory_sample) {
      ++stats.textures_from_resolves;
      return &r;
    }
    // The CPU has put something else there: a texture from memory again.
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 40)
      REXLOG_INFO("[native] frame {}: resolved texture {:08X} ({}x{}, {} bytes) dropped: its "
                  "memory changed",
                  frame, r.memory_address, r.image.width, r.image.height, r.memory_bytes);
    DestroyLater(r.image);
    DestroyLater(r.packed);
    resolved.erase(it);
    ++stats.resolves_overwritten;
  }
  // Identity: everything that shapes the data (not filtering or swizzles).
  const uint32_t key_fields[] = {
      uint32_t(fetch.dimension), uint32_t(fetch.format), uint32_t(fetch.endianness),
      fetch.base_address,        fetch.mip_address,       fetch.dword_2,
      fetch.pitch,               fetch.tiled,             fetch.packed_mips,
      fetch.mip_min_level,       fetch.mip_max_level,     fetch.stacked};
  const uint64_t key = XXH3_64bits(key_fields, sizeof(key_fields));
  TextureEntry& entry = textures[key];
  if (entry.image.texture && entry.validated_frame == frame) return &entry;
  PageWatch& pages = PageWatch::Get();

  // Content hash of the guest memory the texture occupies.
  uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
  xg::texture_util::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1,
                                                     &base_page, &mip_page, &mip_min, &mip_max);
  const auto layout = xg::texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width_m1 + 1, height_m1 + 1, depth_m1 + 1, fetch.tiled,
      fetch.format, fetch.packed_mips, true, mip_max);
  const uint32_t base_size = layout.base.level_data_extent_bytes;
  const uint32_t mip_size = layout.mips_total_extent_bytes;
  // Unchanged memory since it was last checked: no need to hash it again.
  const uint32_t mip_bytes = mip_page && mip_max > 0 ? mip_size : 0;
  if (entry.image.texture && base_page &&
      pages.Unchanged(base_page << 12, base_size, entry.watched_epoch) &&
      (!mip_bytes || pages.Unchanged(mip_page << 12, mip_bytes, entry.watched_epoch))) {
    entry.validated_frame = frame;
    return &entry;
  }
  const uint64_t epoch = pages.Watch(base_page << 12, base_size);
  pages.Watch(mip_page << 12, mip_bytes);
  uint64_t content = 0;
  if (base_page && (base_page << 12) + uint64_t(base_size) <= 0x20000000)
    content = XXH3_64bits(memory + 0xA0000000u + (base_page << 12), base_size);
  if (mip_page && mip_max > 0 && (mip_page << 12) + uint64_t(mip_size) <= 0x20000000)
    content = XXH3_64bits_withSeed(memory + 0xA0000000u + (mip_page << 12), mip_size, content);
  if (entry.image.texture && entry.content_hash == content) {
    entry.validated_frame = frame;
    entry.watched_epoch = epoch;
    return &entry;
  }

  auto failed = [&](const std::string& why) {
    ++stats.texture_failures;
    Note(fmt::format("texture {:08X}: {}", fetch.base_address << 12, why));
    textures.erase(key);
    return nullptr;
  };
  HostTexture host;
  std::string error;
  if (!ConvertTexture(memory, fetch, host, &error)) return failed(error);
  uint64_t staging_offset;
  uint8_t* staging = Allocate(*staging_arena, host.data.size(), 16, staging_offset);
  if (!staging) return failed("staging buffer full");
  std::memcpy(staging, host.data.data(), host.data.size());
  stats.upload_bytes += host.data.size();

  DestroyLater(entry.image);  // changed content: recreate (it may have a new shape)
  Image& image = entry.image;
  image.format = host.format;
  image.is_3d = host.view_type == VK_IMAGE_VIEW_TYPE_3D;
  image.width = host.width, image.height = host.height, image.depth = host.depth;
  image.layers = host.layers;
  image.levels = uint32_t(host.levels.size());
  image.cube = host.view_type == VK_IMAGE_VIEW_TYPE_CUBE;
  if (!CreateImage(image, pl::RenderTextureFlag::NONE)) return failed("texture creation failed");
  Transition(*upload_list, image, pl::RenderTextureLayout::COPY_DEST);
  const pl::RenderFormat format = ToRenderFormat(host.format);
  for (uint32_t level = 0; level < host.levels.size(); ++level) {
    const HostTexture::Level& l = host.levels[level];
    const uint32_t row_texels = l.row_pitch / host.bytes_per_block * host.block_width;
    const uint64_t slice_bytes = uint64_t(l.row_pitch) * l.block_rows;
    for (uint32_t layer = 0; layer < host.layers; ++layer)
      for (uint32_t z = 0; z < l.depth; ++z)  // one slice at a time: rows may be padded
        upload_list->copyTextureRegion(
            pl::RenderTextureCopyLocation::Subresource(image.texture.get(), level, layer),
            pl::RenderTextureCopyLocation::PlacedFootprint(
                staging_arena->buffer.get(), format, l.width, l.height, 1, row_texels,
                staging_offset + l.offset + layer * l.layer_bytes + z * slice_bytes),
            0, 0, z);
  }
  Transition(*upload_list, image, pl::RenderTextureLayout::SHADER_READ);
  entry.content_hash = content;
  entry.validated_frame = frame;
  entry.watched_epoch = epoch;
  ++stats.textures_uploaded;
  return &entry;
}

std::pair<pl::RenderTexture*, pl::RenderTextureView*> Renderer::Impl::TextureView(
    const RegisterFile& regs, uint32_t fetch_index, xenos::FetchOpDimension dimension,
    bool is_signed, bool* scaled_out) {
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fetch_index);
  TextureEntry* entry = GetTexture(fetch);
  *scaled_out = entry && entry->from_resolve;
  // A depth resolve read as color: its memory-format words, viewed in the
  // fetch's format, sampled like a texture loaded from that memory.
  VkFormat view_format = VK_FORMAT_UNDEFINED;
  bool memory_order = entry && !entry->from_resolve;
  if (entry && entry->depth && fetch.format != xenos::TextureFormat::k_24_8 &&
      fetch.format != xenos::TextureFormat::k_24_8_FLOAT) {
    view_format = HostTextureFormat(fetch.format);
    entry->packed_wanted = true;
    if (entry->packed.texture && FormatBytes(view_format) == 4) {
      memory_order = true;
    } else {
      Note(fmt::format("depth resolve {:08X} sampled as format {}: not supported",
                       fetch.base_address << 12, uint32_t(fetch.format)));
      entry = nullptr;
    }
  }
  if (entry) {
    Image& image = view_format != VK_FORMAT_UNDEFINED ? entry->packed : entry->image;
    const uint32_t swizzle =
        memory_order ? GuestToHostSwizzle(fetch.swizzle,
                                          HostFormatSwizzle(view_format != VK_FORMAT_UNDEFINED
                                                                ? view_format
                                                                : image.format))
                     : ResolvedTextureSwizzle(fetch.swizzle, entry->resolve_swizzle);
    pl::RenderTextureView* view = nullptr;
    switch (dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        if (!image.is_3d && !image.cube)
          view = View(image, kView2DArray, swizzle, is_signed, view_format);
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        if (image.is_3d) view = View(image, kView3D, swizzle, is_signed, view_format);
        break;
      case xenos::FetchOpDimension::kCube:
        if (image.cube) view = View(image, kViewCube, swizzle, is_signed, view_format);
        break;
    }
    if (view) return {image.texture.get(), view};
  }
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return {dummy_3d.texture.get(), View(dummy_3d, kView3D, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA)};
    case xenos::FetchOpDimension::kCube:
      return {dummy_cube.texture.get(),
              View(dummy_cube, kViewCube, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA)};
    default:
      return {dummy_2d.texture.get(),
              View(dummy_2d, kView2DArray, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA)};
  }
}

bool Renderer::Impl::LinearFilterable(VkFormat format) {
  if (format == VK_FORMAT_UNDEFINED) return false;
  auto [it, inserted] = linear_filterable.try_emplace(uint32_t(format), false);
  if (inserted) {
    VkFormatProperties properties;
    dev->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(dev->physical_device(),
                                                                            format, &properties);
    it->second = properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  }
  return it->second;
}

// VulkanTextureCache::GetSamplerParameters + UseSampler.
pl::RenderSampler* Renderer::Impl::Sampler(const RegisterFile& regs,
                                           const SpirvShader::SamplerBinding& binding) {
  using Address = pl::RenderTextureAddressMode;
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  const auto& props = dev->properties();
  xenos::ClampMode clamp[3];
  xg::texture_util::GetClampModesForDimension(fetch, clamp[0], clamp[1], clamp[2]);
  Address address[3];
  bool border = false;
  for (int i = 0; i < 3; ++i) {
    switch (clamp[i]) {
      case xenos::ClampMode::kRepeat: address[i] = Address::WRAP; break;
      case xenos::ClampMode::kMirroredRepeat: address[i] = Address::MIRROR; break;
      case xenos::ClampMode::kClampToEdge:
      case xenos::ClampMode::kClampToHalfway:
        address[i] = Address::CLAMP;
        break;
      case xenos::ClampMode::kClampToBorder:
        address[i] = Address::BORDER;
        border = true;
        break;
      default:  // mirror clamp variants
        address[i] = props.samplerMirrorClampToEdge ? Address::MIRROR_ONCE : Address::MIRROR;
    }
  }
  auto pick = [](xenos::TextureFilter shader, xenos::TextureFilter constant) {
    return shader == xenos::TextureFilter::kUseFetchConst ? constant : shader;
  };
  const auto mag = pick(binding.mag_filter, fetch.mag_filter);
  const auto min = pick(binding.min_filter, fetch.min_filter);
  const auto mip = pick(binding.mip_filter, fetch.mip_filter);
  auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                          : binding.aniso_filter;
  if (!props.samplerAnisotropy) aniso = xenos::AnisoFilter::kDisabled;
  uint32_t mip_min, mip_max;
  xg::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr,
                                                     nullptr, &mip_min, &mip_max);
  bool mag_linear = mag == xenos::TextureFilter::kLinear;
  bool min_linear = min == xenos::TextureFilter::kLinear;
  bool mip_linear = mip == xenos::TextureFilter::kLinear;
  const bool base_map = mip == xenos::TextureFilter::kBaseMap;
  if (!LinearFilterable(HostTextureFormat(fetch.format))) {
    mag_linear = min_linear = mip_linear = false;
    aniso = xenos::AnisoFilter::kDisabled;
  }
  // The anisotropic_override setting (as the SDK's texture cache applies it):
  // forced on filtered, mipmapped textures.
  if (anisotropic_override > -1 && anisotropic_override < 6 && mip_max > mip_min && !base_map &&
      mag_linear && min_linear &&
      (mip == xenos::TextureFilter::kPoint || mip == xenos::TextureFilter::kLinear) &&
      props.samplerAnisotropy)
    aniso = xenos::AnisoFilter(anisotropic_override);
  if (aniso != xenos::AnisoFilter::kDisabled) mag_linear = min_linear = mip_linear = true;
  const xenos::BorderColor border_color = border ? fetch.border_color
                                                 : xenos::BorderColor::k_ABGR_Black;
  const uint64_t key = uint64_t(address[0]) | uint64_t(address[1]) << 4 |
                       uint64_t(address[2]) << 8 | uint64_t(border_color) << 12 |
                       uint64_t(mag_linear) << 16 | uint64_t(min_linear) << 17 |
                       uint64_t(mip_linear) << 18 | uint64_t(base_map) << 19 |
                       uint64_t(aniso) << 20 | uint64_t(mip_min) << 24;
  auto [it, inserted] = samplers.try_emplace(key);
  if (!inserted) return it->second.get();
  pl::RenderSamplerDesc desc;
  desc.magFilter = mag_linear ? pl::RenderFilter::LINEAR : pl::RenderFilter::NEAREST;
  desc.minFilter = min_linear ? pl::RenderFilter::LINEAR : pl::RenderFilter::NEAREST;
  desc.mipmapMode = mip_linear ? pl::RenderMipmapMode::LINEAR : pl::RenderMipmapMode::NEAREST;
  desc.addressU = address[0], desc.addressV = address[1], desc.addressW = address[2];
  if (aniso != xenos::AnisoFilter::kDisabled) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = uint32_t(std::min(
        props.maxSamplerAnisotropy,
        float(1u << (uint32_t(aniso) - uint32_t(xenos::AnisoFilter::kMax_1_1)))));
  }
  desc.minLOD = float(mip_min);
  desc.maxLOD = base_map ? desc.minLOD + 0.25f : FLT_MAX;
  desc.borderColor = border_color == xenos::BorderColor::k_ABGR_White
                         ? pl::RenderBorderColor::OPAQUE_WHITE
                         : pl::RenderBorderColor::TRANSPARENT_BLACK;
  it->second = device->createSampler(desc);
  return it->second.get();
}

// --- Layouts, shaders, pipelines -------------------------------------------

SetPool& Renderer::Impl::TexturePool(bool vertex, uint32_t texture_count, uint32_t sampler_count) {
  if (!texture_count && !sampler_count) return empty_pool;
  auto [it, inserted] = texture_pools.try_emplace({vertex, texture_count, sampler_count});
  SetPool& pool = it->second;
  if (inserted) {
    pool.builder.begin();
    for (uint32_t i = 0; i < texture_count; ++i) pool.builder.addTexture(i);
    for (uint32_t i = 0; i < sampler_count; ++i) pool.builder.addSampler(texture_count + i);
    pool.builder.end();
  }
  return pool;
}

pl::RenderPipelineLayout* Renderer::Impl::PipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp,
                                                         uint32_t sp) {
  auto [it, inserted] = pipeline_layouts.try_emplace({tv, sv, tp, sp});
  if (!inserted) return it->second.get();
  // Set numbers as the translator uses them (kDescriptorSet*).
  pl::RenderPipelineLayoutBuilder builder;
  builder.begin();
  builder.addDescriptorSet(shared_pool.builder);
  builder.addDescriptorSet(constants_pool.builder);
  builder.addDescriptorSet(TexturePool(true, tv, sv).builder);
  builder.addDescriptorSet(TexturePool(false, tp, sp).builder);
  builder.end();
  it->second = builder.create(device);
  return it->second.get();
}

pl::RenderShader* Renderer::Impl::Shader(const SpirvShader::Translation& translation) {
  auto [it, inserted] = shaders.try_emplace(&translation);
  if (inserted) {
    const auto& bin = translation.translated_binary();
    it->second = device->createShader(bin.data(), bin.size(), "main", pl::RenderShaderFormat::SPIRV);
  }
  return it->second.get();
}

pl::RenderPipeline* Renderer::Impl::Pipeline(const PipelineKey& k) {
  auto [it, inserted] = pipelines.try_emplace(k);
  if (!inserted) return it->second.get();
  pl::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = static_cast<const pl::RenderPipelineLayout*>(k.layout);
  desc.vertexShader = Shader(*static_cast<const SpirvShader::Translation*>(k.vs));
  desc.pixelShader = k.ps ? Shader(*static_cast<const SpirvShader::Translation*>(k.ps)) : nullptr;
  desc.primitiveTopology = pl::RenderPrimitiveTopology(k.topology);
  desc.cullMode = pl::RenderCullMode(k.cull_mode);
  desc.frontFace = k.front_face_clockwise ? pl::RenderFrontFace::CLOCKWISE
                                          : pl::RenderFrontFace::COUNTER_CLOCKWISE;
  desc.depthClipEnabled = !k.depth_clamp;
  desc.dynamicDepthBiasEnabled = true;
  desc.dynamicStencilEnabled = true;
  desc.dynamicBlendConstantsEnabled = true;
  desc.depthEnabled = k.depth_test;
  desc.depthWriteEnabled = k.depth_write;
  desc.depthFunction = ToCompare(k.depth_compare);
  desc.stencilEnabled = k.stencil_test;
  auto face = [](const uint32_t s[4]) {
    pl::RenderStencilFaceDesc f;
    f.failOp = ToStencilOp(s[0]);
    f.passOp = ToStencilOp(s[1]);
    f.depthFailOp = ToStencilOp(s[2]);
    f.compareFunction = ToCompare(s[3]);
    return f;
  };
  desc.stencilFrontFace = face(k.stencil_front);
  desc.stencilBackFace = face(k.stencil_back);
  desc.multisampling.sampleCount = k.samples;
  desc.renderTargetCount = k.color_count;
  for (uint32_t i = 0; i < k.color_count; ++i) {
    pl::RenderBlendDesc& b = desc.renderTargetBlend[i];
    b.blendEnabled = k.blend[i][0];
    b.srcBlend = kBlendFactors[k.blend[i][1]];
    b.dstBlend = kBlendFactors[k.blend[i][2]];
    b.blendOp = kBlendOps[k.blend[i][3]];
    b.srcBlendAlpha = kBlendFactors[k.blend[i][4]];
    b.dstBlendAlpha = kBlendFactors[k.blend[i][5]];
    b.blendOpAlpha = kBlendOps[k.blend[i][6]];
    b.renderTargetWriteMask = uint8_t(k.write_mask[i]);
    desc.renderTargetFormat[i] = ToRenderFormat(VkFormat(k.color_formats[i]));
  }
  desc.depthTargetFormat = k.depth_format ? ToRenderFormat(VkFormat(k.depth_format))
                                          : pl::RenderFormat::UNKNOWN;
  it->second = device->createGraphicsPipeline(desc);
  if (it->second)
    ++stats.pipelines_created;
  else
    Note("pipeline creation failed");
  return it->second.get();
}

pl::RenderPipeline* Renderer::Impl::ResolvePipeline(VkFormat dest_format, uint32_t samples,
                                                    bool depth) {
  auto [it, inserted] = resolve_pipelines.try_emplace({uint32_t(dest_format), samples, depth});
  if (!inserted) return it->second.get();
  const pl::RenderSpecConstant spec[2] = {{0, samples}, {1, uint32_t(depth)}};
  pl::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = resolve_layout.get();
  desc.vertexShader = resolve_vs_shader.get();
  desc.pixelShader = samples > 1 ? resolve_ms_shader.get() : resolve_shader.get();
  desc.specConstants = samples > 1 ? spec : nullptr;
  desc.specConstantsCount = samples > 1 ? 2 : 0;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = ToRenderFormat(dest_format);
  desc.renderTargetBlend[0] = pl::RenderBlendDesc::Copy();
  it->second = device->createGraphicsPipeline(desc);
  return it->second.get();
}

pl::RenderPipeline* Renderer::Impl::PackPipeline(uint32_t samples) {
  auto [it, inserted] = pack_pipelines.try_emplace(samples);
  if (!inserted) return it->second.get();
  pl::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = pack_layout.get();
  desc.vertexShader = resolve_vs_shader.get();
  desc.pixelShader = samples > 1 ? pack_ms_shader.get() : pack_shader.get();
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = pl::RenderFormat::R32_UINT;
  desc.renderTargetBlend[0] = pl::RenderBlendDesc::Copy();
  it->second = device->createGraphicsPipeline(desc);
  return it->second.get();
}

void Renderer::Impl::DrawResolve(Image& dest, RenderTarget& rt, pl::RenderPipeline* pipeline,
                                 pl::RenderPipelineLayout* layout, pl::RenderDescriptorSet* set,
                                 const pl::RenderRect& scissor, const void* constants,
                                 uint32_t constants_size) {
  pl::RenderCommandList& list = *draw_list;
  Transition(list, rt.image, pl::RenderTextureLayout::SHADER_READ);
  Transition(list, dest, pl::RenderTextureLayout::COLOR_WRITE, /*always=*/true);  // tiles
  list.setFramebuffer(dest.framebuffer.get());
  list.setGraphicsPipelineLayout(layout);
  list.setPipeline(pipeline);
  list.setGraphicsDescriptorSet(set, 0);
  const pl::RenderViewport viewport(0.0f, 0.0f, float(dest.width), float(dest.height));
  list.setViewports(&viewport, 1);
  list.setScissors(&scissor, 1);
  list.setGraphicsPushConstants(0, constants, 0, constants_size);
  list.drawInstanced(3, 1, 0, 0);
  list.setFramebuffer(nullptr);
  ++stats.render_passes;
}

// --- Draws ---------------------------------------------------------------------

void Renderer::Impl::Draw(const RegisterFile& regs, std::span<const uint32_t> vs_ucode,
                          std::span<const uint32_t> ps_ucode, const GuestDraw& draw) {
  const uint32_t n = stats.draws++;
  auto skip = [&](std::string why) {
    ++stats.draws_skipped;
    if (!debug_dir.empty()) frame_log.push_back(fmt::format("{} skipped: {}", n, why));
    Note(fmt::format("draw {}: {}", n, why));
  };
  const auto& props = dev->properties();
  if (n >= skip_first && n < skip_end) return;
  const auto edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) return skip("copy mode");
  // Occlusion query draws: count samples, write nothing (the SDK drops them too).
  if (const auto viz = regs.Get<reg::PA_SC_VIZ_QUERY>(); viz.viz_query_ena && viz.kill_pix_post_hi_z) {
    ++stats.viz_query_draws;
    return;
  }
  if (vs_ucode.empty()) return skip("no vertex shader");

  SpirvShader* vs = library->Get(xenos::ShaderType::kVertex, vs_ucode);
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal))
    return skip("no rasterization (memexport only?)");
  const auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (polygonal && mode_cntl.cull_front && mode_cntl.cull_back) return;  // nothing to draw
  SpirvShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth && !ps_ucode.empty()) {
    ps = library->Get(xenos::ShaderType::kPixel, ps_ucode);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, regs)) ps = nullptr;
  }
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);

  HostDraw host;
  std::string error;
  if (!ConvertDraw(memory, draw, host, &error)) return skip(error);
  if (!host.count) return;

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification).
  ShaderPairState pair;
  pair.sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  pair.sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  pair.sq_interpolator_cntl = regs.Get<reg::SQ_INTERPOLATOR_CNTL>();
  pair.msaa_samples = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  pair.primitive_type = xenos::PrimitiveType(draw.initiator & 0x3F);
  ShaderPairModifications mods = library->Modifications(*vs, ps, pair);
  const auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t ucp = clip_cntl.clip_disable ? 0 : clip_cntl.ucp_ena;
  if (ucp && !props.shaderClipDistance) ucp = 0;
  mods.vertex.vertex.user_clip_plane_count = std::popcount(ucp);
  mods.vertex.vertex.user_clip_plane_cull = 0;
  mods.vertex.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
  mods.vertex.vertex.vertex_kill_and =
      uint32_t(props.shaderCullDistance && (vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !clip_cntl.vtx_kill_or);
  if (ps) {
    using DSM = SpirvShaderTranslator::Modification::DepthStencilMode;
    mods.pixel.pixel.depth_stencil_mode =
        ps->implicit_early_z_write_allowed() &&
                (!ps->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DSM::kEarlyHint
            : DSM::kNoModifiers;
  }
  SpirvShader::Translation* vst = library->Translate(*vs, mods.vertex);
  SpirvShader::Translation* pst = ps ? library->Translate(*ps, mods.pixel) : nullptr;
  if (!vst || (ps && !pst)) return skip("shader translation failed");

  // Viewport and scissor (UpdateDynamicState), in host pixels.
  draw_util::ViewportInfo vi;
  draw_util::GetHostViewportInfo(regs, scale, scale, false, props.maxViewportDimensions[0],
                                 props.maxViewportDimensions[1], true, depth_control, false, true,
                                 ps && ps->writes_depth(), vi);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // The rows the draw can reach, for the render targets' size.
  uint32_t rows = scissor.offset[1] + scissor.extent[1];
  if (vi.xy_extent[1]) rows = std::min(rows, (vi.xy_offset[1] + vi.xy_extent[1] + scale - 1) / scale);
  for (uint32_t i = 0; i < 2; ++i) {
    scissor.offset[i] *= scale;
    scissor.extent[i] *= scale;
  }

  // Render targets.
  const uint32_t color_mask = ps ? draw_util::GetNormalizedColorMask(regs, ps->writes_color_targets())
                                 : 0;
  std::array<RenderTarget*, 4> colors{};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (i * 4)) & 0xF)) continue;
    colors[i] = GetRenderTarget(regs, int(i), rows);
    if (colors[i]) color_count = i + 1;
  }
  for (uint32_t i = 0; i < color_count; ++i)
    if (!colors[i]) return skip(fmt::format("color target {} missing below {}", i, color_count));
  RenderTarget* depth = (depth_control.z_enable || depth_control.stencil_enable)
                            ? GetRenderTarget(regs, -1, rows)
                            : nullptr;
  if (!color_count && !depth) return skip("no render targets");

  // Bindings.
  const auto& vs_textures = vs->GetTextureBindingsAfterTranslation();
  const auto& vs_samplers = vs->GetSamplerBindingsAfterTranslation();
  static const std::vector<SpirvShader::TextureBinding> kNoTextures;
  static const std::vector<SpirvShader::SamplerBinding> kNoSamplers;
  const auto& ps_textures = ps ? ps->GetTextureBindingsAfterTranslation() : kNoTextures;
  const auto& ps_samplers = ps ? ps->GetSamplerBindingsAfterTranslation() : kNoSamplers;
  pl::RenderPipelineLayout* layout =
      PipelineLayout(uint32_t(vs_textures.size()), uint32_t(vs_samplers.size()),
                     uint32_t(ps_textures.size()), uint32_t(ps_samplers.size()));
  if (!layout) return skip("no pipeline layout");

  // Pipeline (VulkanPipelineCache::GetCurrentStateDescription, host RTs).
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vst;
  key.ps = pst;
  key.layout = layout;
  key.topology = uint32_t(ToRenderTopology(host.topology));
  key.depth_clamp = props.depthClamp && clip_cntl.clip_disable;
  key.cull_mode = uint32_t(pl::RenderCullMode::NONE);
  if (polygonal) {
    if (mode_cntl.cull_front) key.cull_mode = uint32_t(pl::RenderCullMode::FRONT);
    if (mode_cntl.cull_back) key.cull_mode = uint32_t(pl::RenderCullMode::BACK);
    key.front_face_clockwise = mode_cntl.face != 0;
    if (props.fillModeNonSolid && mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType type = xenos::PolygonType::kTriangles;
      if (!mode_cntl.cull_front) type = std::min(type, mode_cntl.polymode_front_ptype);
      if (!mode_cntl.cull_back) type = std::min(type, mode_cntl.polymode_back_ptype);
      if (type != xenos::PolygonType::kTriangles)
        Note(fmt::format("draw {}: line polygon mode drawn filled", n));
    }
  }
  if (depth) {
    key.depth_format = uint32_t(kDepthFormat);
    if (depth_control.z_enable) {
      key.depth_test = 1;
      key.depth_write = depth_control.z_write_enable;
      key.depth_compare = uint32_t(depth_control.zfunc);
    } else {
      key.depth_compare = uint32_t(xenos::CompareFunction::kAlways);
    }
    if (depth_control.stencil_enable) {
      key.stencil_test = 1;
      key.stencil_front[0] = uint32_t(depth_control.stencilfail);
      key.stencil_front[1] = uint32_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint32_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint32_t(depth_control.stencilfunc);
      if (polygonal && depth_control.backface_enable) {
        key.stencil_back[0] = uint32_t(depth_control.stencilfail_bf);
        key.stencil_back[1] = uint32_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] = uint32_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] = uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  key.color_count = color_count;
  for (uint32_t i = 0; i < color_count; ++i) {
    key.color_formats[i] = uint32_t(colors[i]->image.format);
    const uint32_t mask = (color_mask >> (i * 4)) & 0xF;
    key.write_mask[i] = mask;
    const auto bc = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegs[i]);
    const uint32_t f[6] = {uint32_t(bc.color_srcblend), uint32_t(bc.color_destblend),
                           uint32_t(bc.color_comb_fcn), uint32_t(bc.alpha_srcblend),
                           uint32_t(bc.alpha_destblend), uint32_t(bc.alpha_comb_fcn)};
    const bool off = f[0] == 1 && f[1] == 0 && f[2] == 0 && f[3] == 1 && f[4] == 0 && f[5] == 0;
    key.blend[i][0] = mask && !off;
    for (int j = 0; j < 6; ++j) key.blend[i][j + 1] = mask ? f[j] & (j == 2 || j == 5 ? 7 : 31) : 0;
    if (!mask) key.blend[i][1] = key.blend[i][4] = 1;  // ONE / ZERO: valid, unused
  }
  key.samples = depth ? depth->image.samples : 1;
  for (uint32_t i = 0; i < color_count; ++i) key.samples = colors[i]->image.samples;
  pl::RenderPipeline* pipeline = Pipeline(key);
  if (!pipeline) return skip("no pipeline");

  // The system constants (UpdateSystemConstantValues).
  SpirvShaderTranslator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (vte.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  flags |= uint32_t(surface.msaa_samples) << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8)
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  const auto alpha_func = colorcontrol.alpha_test_enable ? colorcontrol.alpha_func
                                                         : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]);
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    float bias;
    const int32_t exp_bias = ci.color_exp_bias;
    const uint32_t bits = uint32_t(0x3F800000 + (exp_bias << 23));
    std::memcpy(&bias, &bits, 4);
    sc.color_exp_bias[i] = bias;
  }
  sc.flags = flags;
  sc.vertex_index_endian = xenos::Endian::kNone;  // ConvertDraw swapped the indices
  sc.line_loop_closing_index = UINT32_MAX;
  sc.vertex_base_index = regs.Get<int32_t>(xg::XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = regs[xg::XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.vertex_index_max = regs[xg::XE_GPU_REG_VGT_MAX_VTX_INDX];
  {
    float* plane = sc.user_clip_planes[0];
    for (uint32_t i = 0; i < 6; ++i) {
      if (!(ucp & (1u << i))) continue;
      std::memcpy(plane, &regs[xg::XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 4 * sizeof(float));
      plane += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = vi.ndc_scale[i];
    sc.ndc_offset[i] = vi.ndc_offset[i];
  }
  if (pair.primitive_type == xenos::PrimitiveType::kPointList) {
    const auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] = float(scale) / std::max(vi.xy_extent[0], 1u);
    sc.point_screen_diameter_to_ndc_radius[1] = float(scale) / std::max(vi.xy_extent[1], 1u);
  }
  const uint32_t used_textures =
      vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  for (uint32_t t = 0; t < 32; ++t) {
    if (!(used_textures & (1u << t))) continue;
    const uint8_t signs = xg::texture_util::SwizzleSigns(regs.GetTextureFetch(t));
    sc.texture_swizzled_signs[t >> 2] |= uint32_t(signs) << (8 * (t & 3));
  }
  sc.alpha_test_reference = regs.Get<float>(xg::XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask =
      colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1u << 8) : 0;

  // Vertex data: each vertex fetch constant the shader uses gets a copy of its
  // buffer in the vertex arena, and the copy of the fetch constants points there.
  uint32_t fetch_constants[32 * 6];
  std::memcpy(fetch_constants, &regs[xg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(fetch_constants));
  for (const auto& binding : vs->vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(binding.fetch_constant);
    const uint32_t address = vf.address << 2, bytes = vf.size << 2;
    if (!bytes || uint64_t(address) + bytes > 0x20000000) continue;
    const uint8_t* src = memory + 0xA0000000u + address;
    PageWatch& pages = PageWatch::Get();
    const uint64_t key = pages.attached()
                             ? uint64_t(address) << 32 | bytes
                             : XXH3_64bits(src, bytes) ^ (uint64_t(address) << 32) ^ bytes;
    uint64_t offset;
    auto it = vertex_copies.find(key);
    if (it != vertex_copies.end() &&
        (!pages.attached() || pages.Unchanged(address, bytes, it->second.second))) {
      offset = it->second.first;
    } else {
      const uint64_t epoch = pages.Watch(address, bytes);
      uint8_t* dst = Allocate(*vertex_arena, bytes, 256, offset);
      if (!dst) return skip("vertex arena full");
      std::memcpy(dst, src, bytes);
      stats.vertex_bytes += bytes;
      vertex_copies[key] = {offset, epoch};
    }
    uint32_t& dword0 = fetch_constants[binding.fetch_constant * 2];
    dword0 = (dword0 & 3) | uint32_t(offset);
  }

  // Texture and sampler descriptors (textures may upload; resolved textures
  // sampled here were made readable at their resolve). Before the system
  // constants: they say which textures are resolution-scaled.
  pl::RenderDescriptorSet* texture_sets[2] = {};
  uint32_t textures_scaled = 0;
  std::string texture_log;  // debug_dir
  for (int stage = 0; stage < 2; ++stage) {
    const auto& stage_textures = stage ? ps_textures : vs_textures;
    const auto& stage_samplers = stage ? ps_samplers : vs_samplers;
    if (stage_textures.empty() && stage_samplers.empty()) continue;
    pl::RenderDescriptorSet* set =
        TexturePool(stage == 0, uint32_t(stage_textures.size()), uint32_t(stage_samplers.size()))
            .Next(device, context_index);
    texture_sets[stage] = set;
    for (uint32_t i = 0; i < stage_textures.size(); ++i) {
      const auto& t = stage_textures[i];
      stats.signed_texture_bindings += t.is_signed;
      bool scaled = false;
      auto [texture, view] =
          TextureView(regs, t.fetch_constant, t.dimension, t.is_signed, &scaled);
      set->setTexture(i, texture, pl::RenderTextureLayout::SHADER_READ, view);
      if (scaled && scale > 1) textures_scaled |= 1u << t.fetch_constant;
      if (!debug_dir.empty()) {
        const auto fetch = regs.GetTextureFetch(t.fetch_constant);
        texture_log += fmt::format(" t{} {:08X}/f{}{}", t.fetch_constant, fetch.base_address << 12,
                                   uint32_t(fetch.format),
                                   !texture ? "(none)" : scaled ? "(resolved)" : "");
      }
    }
    for (uint32_t i = 0; i < stage_samplers.size(); ++i)
      set->setSampler(uint32_t(stage_textures.size()) + i, Sampler(regs, stage_samplers[i]));
  }

  // Constants: one set per draw, its buffers at this draw's offsets.
  const uint64_t ubo_align = props.minUniformBufferOffsetAlignment;
  pl::RenderDescriptorSet* constants_set = constants_pool.Next(device, context_index);
  auto upload = [&](uint32_t index, const void* data, size_t size) {
    uint64_t offset;
    // Room for the descriptor's full range (constant ranges are fixed).
    uint8_t* dst =
        Allocate(*uniform_arena, std::max<uint64_t>(size, uniform_ranges[index]), ubo_align, offset);
    if (!dst) return false;
    std::memcpy(dst, data, size);
    stats.uniform_bytes += size;
    // A structured view of 1-byte elements is how Plume takes a buffer offset.
    const pl::RenderBufferStructuredView at(1, uint32_t(offset));
    constants_set->setBuffer(index, uniform_arena->buffer.get(), uniform_ranges[index], &at);
    return true;
  };
  auto pack_floats = [&](const SpirvShader& shader, uint32_t first_register) {
    std::vector<uint32_t> packed;
    const auto& map = shader.constant_register_map();
    for (uint32_t i = 0; i < 4; ++i)
      for (uint32_t b = 0; b < 64; ++b)
        if (map.float_bitmap[i] & (uint64_t(1) << b)) {
          const uint32_t* c = &regs[first_register + (i * 64 + b) * 4];
          packed.insert(packed.end(), c, c + 4);
        }
    if (packed.empty()) packed.resize(4);
    return packed;
  };
  const auto vs_floats = pack_floats(*vs, xg::XE_GPU_REG_SHADER_CONSTANT_000_X);
  if (!debug_dir.empty()) {
    std::string line = fmt::format(
        "{} vs {:016X} ps {:016X} prim {} n {} ib {:08X}/{} vp {},{} {}x{} sc {},{} {}x{} rt {} ds {} "
        "vsc {:016X} rt0 {:X}/fmt{} mask {:X} blend {:08X}",
        n, vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0, uint32_t(pair.primitive_type),
        host.count, draw.index_base, draw.index_size, vi.xy_offset[0], vi.xy_offset[1],
        vi.xy_extent[0], vi.xy_extent[1], scissor.offset[0], scissor.offset[1], scissor.extent[0],
        scissor.extent[1], colors[0] ? colors[0]->image.width : 0, depth ? depth->image.width : 0,
        XXH3_64bits(vs_floats.data(), vs_floats.size() * 4), colors[0] ? colors[0]->base : 0,
        colors[0] ? colors[0]->format : 0, regs[xg::XE_GPU_REG_RB_COLOR_MASK],
        regs[xg::XE_GPU_REG_RB_BLENDCONTROL0]);
    for (const auto& binding : vs->vertex_bindings()) {
      const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(binding.fetch_constant);
      const uint32_t address = vf.address << 2, bytes = vf.size << 2;
      line += fmt::format(" vb{} {:08X}+{:X}", binding.fetch_constant, address, bytes);
      if (bytes && uint64_t(address) + bytes <= 0x20000000)
        line += fmt::format("#{:016X}", XXH3_64bits(memory + 0xA0000000u + address, bytes));
    }
    frame_log.push_back(std::move(line));
  }
  if (!debug_dir.empty() && !frame_log.empty()) frame_log.back() += texture_log;
  const auto ps_floats =
      ps ? pack_floats(*ps, xg::XE_GPU_REG_SHADER_CONSTANT_256_X) : std::vector<uint32_t>(4);
  if (!debug_dir.empty() && !frame_log.empty()) {
    // Pixel shader constants (first 32), and each pixel shader's microcode once.
    std::string& line = frame_log.back();
    line += " psc";
    for (size_t i = 0; i < std::min<size_t>(ps_floats.size(), 32); ++i) {
      float f;
      std::memcpy(&f, &ps_floats[i], 4);
      line += fmt::format(" {:g}", f);
    }
    if (ps && dumped_shaders.insert(ps->ucode_data_hash()).second) {
      std::ofstream out(debug_dir + fmt::format("/ps_{:016X}.txt", ps->ucode_data_hash()));
      out << ps->ucode_disassembly();
    }
  }
  sc.textures_resolution_scaled = textures_scaled;
  if (!upload(SpirvShaderTranslator::kConstantBufferSystem, &sc, sizeof(sc)) ||
      !upload(SpirvShaderTranslator::kConstantBufferFloatVertex, vs_floats.data(),
              vs_floats.size() * 4) ||
      !upload(SpirvShaderTranslator::kConstantBufferFloatPixel, ps_floats.data(),
              ps_floats.size() * 4) ||
      !upload(SpirvShaderTranslator::kConstantBufferBoolLoop,
              &regs[xg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * 4) ||
      !upload(SpirvShaderTranslator::kConstantBufferFetch, fetch_constants, sizeof(fetch_constants)))
    return skip("uniform arena full");

  // Indices.
  uint64_t index_offset = 0;
  if (host.indexed) {
    uint8_t* dst = Allocate(*vertex_arena, host.indices.size(), 4, index_offset);
    if (!dst) return skip("vertex arena full");
    std::memcpy(dst, host.indices.data(), host.indices.size());
    stats.vertex_bytes += host.indices.size();
  }

  if (!BeginRendering(colors, depth)) return skip("render pass");
  pl::RenderCommandList& list = *draw_list;
  list.setGraphicsPipelineLayout(layout);
  list.setPipeline(pipeline);
  list.setGraphicsDescriptorSet(shared_set, SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram);
  list.setGraphicsDescriptorSet(constants_set, SpirvShaderTranslator::kDescriptorSetConstants);
  for (int stage = 0; stage < 2; ++stage)
    if (texture_sets[stage])
      list.setGraphicsDescriptorSet(texture_sets[stage],
                                    SpirvShaderTranslator::kDescriptorSetTexturesVertex + stage);
  pl::RenderViewport viewport(-1.0f, -1.0f, 1.0f, 1.0f, vi.z_min, vi.z_max);
  if (vi.xy_extent[0] && vi.xy_extent[1])
    viewport = pl::RenderViewport(float(vi.xy_offset[0]), float(vi.xy_offset[1]),
                                  float(vi.xy_extent[0]), float(vi.xy_extent[1]), vi.z_min,
                                  vi.z_max);
  list.setViewports(&viewport, 1);
  const pl::RenderRect scissor_rect(
      int32_t(scissor.offset[0]), int32_t(scissor.offset[1]),
      int32_t(scissor.offset[0] + scissor.extent[0]), int32_t(scissor.offset[1] + scissor.extent[1]));
  list.setScissors(&scissor_rect, 1);
  float bias_slope, bias_constant;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, bias_slope, bias_constant);
  bias_constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                       ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                       : draw_util::kD3D10PolygonOffsetFactorFloat24;
  bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit * float(scale);
  list.setDepthBias(bias_constant, 0.0f, bias_slope);
  const float blend_constants[4] = {
      regs.Get<float>(xg::XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(xg::XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(xg::XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(xg::XE_GPU_REG_RB_BLEND_ALPHA)};
  list.setBlendConstants(blend_constants);
  auto ref_front = regs.Get<reg::RB_STENCILREFMASK>(xg::XE_GPU_REG_RB_STENCILREFMASK);
  auto ref_back = polygonal && depth_control.backface_enable
                      ? regs.Get<reg::RB_STENCILREFMASK>(xg::XE_GPU_REG_RB_STENCILREFMASK_BF)
                      : ref_front;
  list.setStencilState(ref_front.stencilref, ref_back.stencilref, ref_front.stencilmask,
                       ref_back.stencilmask, ref_front.stencilwritemask, ref_back.stencilwritemask);
  if (host.indexed) {
    const pl::RenderIndexBufferView index_view(
        pl::RenderBufferReference(vertex_arena->buffer.get(), index_offset),
        uint32_t(host.indices.size()),
        host.index32 ? pl::RenderFormat::R32_UINT : pl::RenderFormat::R16_UINT);
    list.setIndexBuffer(&index_view);
    list.drawIndexedInstanced(host.count, 1, 0, 0, 0);
  } else {
    list.drawInstanced(host.count, 1, 0, 0);
  }
}

}  // namespace tb2::native

namespace tb2::native {

// --- Public interface ------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::Create(const VulkanDevice* device, uint32_t resolution_scale,
                                           std::string* error) {
  std::unique_ptr<Renderer> r(new Renderer());
  r->impl_->scale = std::clamp<uint32_t>(resolution_scale, 1, 4);
  if (!r->impl_->Init(device, error)) return nullptr;
  return r;
}

const RendererStats& Renderer::stats() const { return impl_->stats; }

std::string Renderer::DeviceName() const { return impl_->device->getDescription().name; }

void Renderer::BeginFrame(const uint8_t* memory, bool readback, bool continuous) {
  Impl& m = *impl_;
  m.memory = memory;
  m.readback = readback;
  ++m.frame;
  m.anisotropic_override = rex::cvar::Query<int32_t>("anisotropic_override");
  if (rex::cvar::GetFlagByName("anisotropic_override").empty()) m.anisotropic_override = -1;
  m.stats = RendererStats{};

  // The next frame context, once the GPU is done with it.
  m.context_index = (m.context_index + 1) % kFramesInFlight;
  Impl::FrameContext& c = m.Context();
  if (c.submitted) {
    m.queue->waitForCommandFence(c.fence.get());
    c.submitted = false;
    if (c.timestamps && m.timestamp_period_ns > 0) {
      c.timestamps->queryResults();
      const uint64_t* t = c.timestamps->getResults();
      // Plume converts the ticks to nanoseconds.
      auto ms = [](uint64_t a, uint64_t b) { return float(double(b - a) * 1e-6); };
      m.stats.gpu_ms_previous = ms(t[0], t[1]);
      uint64_t last = t[0];
      for (uint32_t i = 0; i < c.resolve_stamps; ++i) {
        m.stats.gpu_pass_ms.push_back(ms(last, t[2 + i]));
        last = t[2 + i];
      }
      m.stats.gpu_pass_ms.push_back(ms(last, t[1]));
    }
  }
  if (c.probed) {
    c.probed = false;
    const auto* texels = static_cast<const uint8_t*>(c.probe->map());
    const size_t count = (texels ? m.probe_bytes : 0) / 4;
    double sum = 0;
    for (size_t i = 0; i < count; ++i)
      sum += texels[i * 4] * 0.3 + texels[i * 4 + 1] * 0.59 + texels[i * 4 + 2] * 0.11;
    if (texels) c.probe->unmap();
    const double mean = count ? sum / double(count) : 0;
    if (c.log.size() < 1000) {
      // not gameplay: ignored
    } else if (m.probe_average > 5 && mean > m.probe_average + 12 && m.probe_dumps < 200) {
      ++m.probe_dumps;
      std::ofstream out(m.debug_dir + fmt::format("/native_bright_{}.txt", c.probe_frame));
      out << fmt::format("# frame {} patch mean {:.1f}, usual {:.1f}\n", c.probe_frame, mean,
                         m.probe_average);
      for (const auto& l : c.log) out << l << '\n';
      std::ofstream normal(m.debug_dir + fmt::format("/native_normal_{}.txt", c.probe_frame));
      for (const auto& l : m.last_normal_log) normal << l << '\n';
    } else if (mean <= (m.probe_average < 0 ? 1e9 : m.probe_average + 12)) {
      m.probe_average = m.probe_average < 0 ? mean : m.probe_average * 0.9 + mean * 0.1;
      m.last_normal_log = c.log;
    }
  }
  std::string small_values;
  for (Readback& rb : c.checks) {
    const auto* p = static_cast<const uint8_t*>(rb.buffer->map());
    if (!p) continue;
    const size_t texels = size_t(rb.result.width) * rb.result.height;
    const uint32_t bad = CountNonFinite(rb.format, p, texels);
    // The first texel, for the black-frame log.
    small_values += fmt::format(" {}:{:08X}=", rb.result.index, rb.result.dest_base);
    const uint32_t comps = rb.format == VK_FORMAT_R16_SFLOAT || rb.format == VK_FORMAT_R32_SFLOAT ? 1
                           : rb.format == VK_FORMAT_R16G16_SFLOAT || rb.format == VK_FORMAT_R32G32_SFLOAT ? 2
                                                                                                         : 4;
    for (uint32_t k = 0; k < comps; ++k) {
      float v;
      if (FormatBytes(rb.format) / comps == 2) {
        uint16_t h;
        std::memcpy(&h, p + k * 2, 2);
        v = HalfToFloat(h);
      } else {
        std::memcpy(&v, p + k * 4, 4);
      }
      small_values += fmt::format("{}{:g}", k ? "," : "", v);
    }
    rb.buffer->unmap();
    static std::atomic<int> logged{0};
    if (bad && logged.fetch_add(1) < 50)
      REXLOG_WARN("[native] frame {} resolve {} -> {:08X} ({}x{}, format {}): {} non-finite "
                  "values (an inf / NaN in the luminance chain blacks out the scene)",
                  c.checks_frame, rb.result.index, rb.result.dest_base, rb.result.width,
                  rb.result.height, uint32_t(rb.format), bad);
  }
  c.checks.clear();
  // The stage probes' mean first component (red, or the only one).
  std::string stage_values;
  std::vector<Impl::FrameContext::StageProbe> stage_info;
  if (!c.stage_probes_done.empty()) {
    if (const auto* p = static_cast<const uint8_t*>(c.stage_probe->map())) {
      for (size_t i = 0; i < c.stage_probes_done.size(); ++i) {
        const auto& sp = c.stage_probes_done[i];
        const uint8_t* t = p + i * Impl::kStageProbeBytes;
        const uint32_t bytes = FormatBytes(sp.format);
        double sum = 0;
        bool known = true;
        for (uint32_t k = 0; k < 64; ++k) {
          const uint8_t* q = t + k * bytes;
          switch (sp.format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8_UNORM:
              sum += q[0] / 255.0;
              break;
            case VK_FORMAT_B8G8R8A8_UNORM:
              sum += q[2] / 255.0;
              break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
              uint32_t v;
              std::memcpy(&v, q, 4);
              sum += (v & 0x3FF) / 1023.0;
              break;
            }
            case VK_FORMAT_R16_SFLOAT:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
              uint16_t h;
              std::memcpy(&h, q, 2);
              sum += HalfToFloat(h);
              break;
            }
            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT: {
              float f;
              std::memcpy(&f, q, 4);
              sum += f;
              break;
            }
            default:
              known = false;
          }
        }
        stage_values += known ? fmt::format(" {}:{:08X}={:.3g}", sp.index, sp.dest_base, sum / 64)
                              : fmt::format(" {}:{:08X}=(format {})", sp.index, sp.dest_base,
                                            uint32_t(sp.format));
      }
      c.stage_probe->unmap();
    }
    stage_info = std::move(c.stage_probes_done);
    c.stage_probes_done.clear();
  }
  if (c.dark_probed) {
    c.dark_probed = false;
    if (const auto* p = static_cast<const uint8_t*>(c.dark_probe->map())) {
      double sum = 0;
      for (uint32_t i = 0; i < 16 * 64; ++i)
        sum += p[i * 4] * 0.3 + p[i * 4 + 1] * 0.59 + p[i * 4 + 2] * 0.11;
      c.dark_probe->unmap();
      const double mean = sum / (16 * 64);
      static std::atomic<int> logged{0};
      // A gameplay frame (hundreds of draws: not a menu or a fade) much darker
      // than the one before.
      if (m.dark_average > 25 && mean < 6 && c.dark_draws >= 300) {
        if (logged.fetch_add(1) < 50)
          REXLOG_WARN("[native] frame {}: presented frame almost black (brightness {:.1f}, usual "
                      "{:.1f}); {} draws; small resolves:{} (previous frame:{}); resolves:{}; resolve "
                      "patches:{} (previous frame:{})",
                      c.dark_frame, mean, m.dark_average, c.dark_draws, small_values,
                      m.previous_small_values, c.dark_resolves, stage_values,
                      m.previous_stage_values);
        // With native_debug_frames: that frame's draw log and the one before.
        if (!m.debug_dir.empty() && !c.log.empty() && m.dark_dumps < 20) {
          ++m.dark_dumps;
          std::ofstream out(m.debug_dir + fmt::format("/native_dark_{}.txt", c.dark_frame));
          for (const auto& l : c.log) out << l << '\n';
          std::ofstream before(m.debug_dir + fmt::format("/native_dark_{}_before.txt", c.dark_frame));
          for (const auto& l : m.previous_frame_log) before << l << '\n';
          // A second later, the guest memory of the frame's color resolves
          // (with readback_resolve: the emulator's copies, for comparison).
          m.memory_dump_frame = m.frame + 60;
          m.memory_dump_name = c.dark_frame;
          m.memory_dump_targets = stage_info;
        }
      } else {
        m.dark_average = mean;
      }
    }
  }
  m.previous_small_values = std::move(small_values);
  m.previous_stage_values = std::move(stage_values);
  if (m.memory_dump_frame && m.frame >= m.memory_dump_frame && m.memory) {
    m.memory_dump_frame = 0;
    for (const auto& t : m.memory_dump_targets) {
      // Tiled textures fill whole 32x32 tiles.
      const uint32_t bytes =
          ((t.width + 31) & ~31u) * ((t.height + 31) & ~31u) * FormatBytes(t.format);
      if (uint64_t(t.dest_base) + bytes > 0x20000000) continue;
      std::ofstream out(m.debug_dir + fmt::format("/mem_{}_{}_{:08X}_{}x{}_f{}.bin",
                                                  m.memory_dump_name, t.index, t.dest_base,
                                                  t.width, t.height, uint32_t(t.format)),
                        std::ios::binary);
      out.write(reinterpret_cast<const char*>(m.memory + 0xA0000000u + t.dest_base), bytes);
    }
  }
  if (!c.log.empty()) m.previous_frame_log = std::move(c.log);
  c.log.clear();
  c.checks_frame = m.frame;
  c.garbage_framebuffers.clear();
  c.garbage.clear();
  c.output_framebuffers.clear();
  c.outputs.clear();
  c.vertex_arena.used = c.uniform_arena.used = c.staging_arena.used = 0;
  m.upload_list = c.upload_list.get();
  m.draw_list = c.draw_list.get();
  m.vertex_arena = &c.vertex_arena;
  m.uniform_arena = &c.uniform_arena;
  m.staging_arena = &c.staging_arena;
  m.shared_set = c.shared_set;
  for (SetPool* pool :
       {&m.constants_pool, &m.resolve_pool, &m.pack_pool, &m.present_pool, &m.fxaa_pool})
    pool->used[m.context_index] = 0;
  for (auto& [k, pool] : m.texture_pools) pool.used[m.context_index] = 0;
  m.vertex_copies.clear();
  m.frame_resolves.clear();
  c.stage_probes.clear();
  if (!continuous) {
    // Resolved textures of an earlier frame no longer stand for their memory.
    for (auto& [k, r] : m.resolved) m.DestroyLater(r.image), m.DestroyLater(r.packed);
    m.resolved.clear();
  }
  m.upload_list->begin();
  m.draw_list->begin();
  if (c.timestamps && m.timestamp_period_ns > 0) {
    m.upload_list->resetQueryPool(c.timestamps.get(), 0, 2 + kResolveStamps);
    c.resolve_stamps = 0;
    m.upload_list->writeTimestamp(c.timestamps.get(), 0);
  }
  m.rendering = false;
  m.pass_colors = {};
  m.pass_depth = nullptr;

  // Placeholder textures: zeroed on first use.
  for (Image* d : {&m.dummy_2d, &m.dummy_cube, &m.dummy_3d}) {
    if (d->layout == pl::RenderTextureLayout::SHADER_READ) continue;
    uint64_t offset;
    uint8_t* zero = m.Allocate(*m.staging_arena, 4, 16, offset);
    std::memset(zero, 0, 4);
    m.Transition(*m.upload_list, *d, pl::RenderTextureLayout::COPY_DEST);
    for (uint32_t layer = 0; layer < d->layers; ++layer)
      m.upload_list->copyTextureRegion(
          pl::RenderTextureCopyLocation::Subresource(d->texture.get(), 0, layer),
          pl::RenderTextureCopyLocation::PlacedFootprint(m.staging_arena->buffer.get(),
                                                         pl::RenderFormat::R8G8B8A8_UNORM, 1, 1,
                                                         1, 1, offset));
    m.Transition(*m.upload_list, *d, pl::RenderTextureLayout::SHADER_READ);
  }
  m.frame_open = true;
}

void Renderer::Draw(const RegisterFile& regs, std::span<const uint32_t> vs_ucode,
                    std::span<const uint32_t> ps_ucode, const GuestDraw& draw) {
  if (impl_->frame_open) impl_->Draw(regs, vs_ucode, ps_ucode, draw);
}

void Renderer::Clear(const RegisterFile& regs, uint32_t flags, uint32_t color, float depth,
                     uint32_t stencil, uint32_t bound) {
  Impl& m = *impl_;
  if (!m.frame_open) return;
  ++m.stats.clears;
  const float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                      float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i)) || !(bound & (1u << i))) continue;
    if (RenderTarget* rt = m.GetRenderTarget(regs, int(i)))
      m.ClearRegion(*rt, nullptr, c, 0.0f, 0, false, false);
  }
  if ((flags & 0x30) && (bound & 0x10))
    if (RenderTarget* rt = m.GetRenderTarget(regs, -1))
      m.ClearRegion(*rt, nullptr, c, depth, stencil, flags & 0x10, flags & 0x20);
}

void Renderer::Resolve(const RegisterFile& regs, uint32_t flags, const int32_t* rect,
                       const int32_t* dest_point, const uint32_t dest_fetch[6],
                       const float clear_color[4], float clear_depth, uint32_t clear_stencil) {
  Impl& m = *impl_;
  if (!m.frame_open) return;
  const uint32_t index = m.stats.resolves++;
  m.EndRendering();
  xenos::xe_gpu_texture_fetch_t dest;
  std::memcpy(&dest, dest_fetch, sizeof(dest));
  uint32_t dw_m1 = 0, dh_m1 = 0;
  xg::texture_util::GetSubresourcesFromFetchConstant(dest, &dw_m1, &dh_m1, nullptr, nullptr, nullptr,
                                                     nullptr, nullptr);
  const uint32_t dw = dw_m1 + 1, dh = dh_m1 + 1;
  const bool depth = flags & 4;
  // Depth goes into a float texture, as the SDK's texture cache loads 24-bit
  // depth.
  const VkFormat dest_format = depth ? VK_FORMAT_R32_SFLOAT : HostTextureFormat(dest.format);
  ResolvedImage result;
  result.index = index;
  result.dest_base = dest.base_address << 12;
  // Source rectangle, and where it goes in the destination (predicated tiling
  // resolves each tile to its place); then in host pixels. The destination
  // texture is resolution-scaled too: tw x th.
  int32_t x0 = 0, y0 = 0, x1 = int32_t(dw), y1 = int32_t(dh);
  if (rect) x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
  RenderTarget* rt =
      m.GetRenderTarget(regs, depth ? -1 : int(flags & 3), uint32_t(std::clamp(y1, 0, 2048)));
  // Diagnostics: a resolve of a surface nothing drew this frame - EDRAM
  // aliasing (drawn through another surface description) or stale content.
  if (rt && rt->written_frame + 1 < m.frame) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 60) {
      std::string drawn;
      for (const auto& [k, t] : m.render_targets)
        if (t.written_frame == m.frame)
          drawn += fmt::format(" {}{:X}/fmt{}/pitch{}/{}x", t.depth ? "D" : "C", t.base, t.format,
                               t.pitch, t.samples);
      REXLOG_INFO("[native] frame {} resolve {}: source {}{:X}/fmt{}/pitch{}/{}x not drawn this "
                  "frame (last {}); drawn:{}",
                  m.frame, index, rt->depth ? "D" : "C", rt->base, rt->format, rt->pitch,
                  rt->samples, rt->written_frame, drawn);
    }
  }
  const int32_t s = int32_t(m.scale);
  x0 *= s, y0 *= s, x1 *= s, y1 *= s;
  const uint32_t tw = dw * m.scale, th = dh * m.scale;
  const int32_t px = dest_point ? std::max(dest_point[0], 0) * s : 0;
  const int32_t py = dest_point ? std::max(dest_point[1], 0) * s : 0;
  if (rt) {
    x0 = std::clamp(x0, 0, int32_t(rt->image.width));
    y0 = std::clamp(y0, 0, int32_t(rt->image.height));
    x1 = std::clamp(x1, x0, std::min(int32_t(rt->image.width), x0 + int32_t(tw) - px));
    y1 = std::clamp(y1, y0, std::min(int32_t(rt->image.height), y0 + int32_t(th) - py));
  }
  const uint32_t w = rt ? uint32_t(std::max(x1 - x0, 0)) : 0;
  const uint32_t h = rt ? uint32_t(std::max(y1 - y0, 0)) : 0;

  // The destination texture: updated in place, like the memory it stands for
  // (commands run in order, so earlier draws still sample what they should).
  TextureEntry* entry = nullptr;
  if (!rt) {
    result.note = "no source render target";
  } else if (ToRenderFormat(dest_format) == pl::RenderFormat::UNKNOWN ||
             IsBlockCompressed(dest_format)) {
    result.note = "no texture for the destination format";
  } else {
    entry = &m.resolved[dest.base_address];
    Image& image = entry->image;
    if (image.texture && (image.format != dest_format || image.width != tw || image.height != th))
      m.DestroyLater(image);
    if (!image.texture) {
      image.format = dest_format;
      image.width = tw, image.height = th;
      const pl::RenderTexture* attachment = nullptr;
      if (m.CreateImage(image, pl::RenderTextureFlag::RENDER_TARGET)) {
        attachment = image.texture.get();
        image.framebuffer = m.device->createFramebuffer(pl::RenderFramebufferDesc(&attachment, 1));
      }
      if (!image.framebuffer) {
        m.resolved.erase(dest.base_address);
        entry = nullptr;
        result.note = "texture creation failed";
      }
    }
  }
  if (entry) {
    entry->from_resolve = true;
    entry->resolve_swizzle = dest.swizzle;
    {
      uint32_t base_page;
      xg::texture_util::GetSubresourcesFromFetchConstant(dest, nullptr, nullptr, nullptr, &base_page,
                                                         nullptr, nullptr, nullptr);
      const auto layout = xg::texture_util::GetGuestTextureLayout(
          dest.dimension, dest.pitch, dw, dh, 1, dest.tiled, dest.format, dest.packed_mips, false, 0);
      entry->memory_address = base_page << 12;
      entry->memory_bytes = layout.base.level_data_extent_bytes;
      entry->memory_sample = SampleMemory(m.memory, entry->memory_address, entry->memory_bytes);
    }
    Image& image = entry->image;
    pl::RenderCommandList& list = *m.draw_list;
    // Depth resolves also keep the packed memory words.
    entry->depth = depth;
    Image& packed = entry->packed;
    if (packed.texture && (!depth || packed.width != tw || packed.height != th))
      m.DestroyLater(packed);
    if (depth && entry->packed_wanted && !packed.texture) {
      packed.format = VK_FORMAT_R32_UINT;
      packed.mutable_format = true;
      packed.width = tw, packed.height = th;
      const pl::RenderTexture* attachment = nullptr;
      if (m.CreateImage(packed, pl::RenderTextureFlag::RENDER_TARGET)) {
        attachment = packed.texture.get();
        packed.framebuffer = m.device->createFramebuffer(pl::RenderFramebufferDesc(&attachment, 1));
      }
      if (!packed.framebuffer) {
        packed = Image{};
        result.note = "packed depth texture creation failed";
      }
    }
    if (w && h) {
      const pl::RenderRect scissor(px, py, px + int32_t(w), py + int32_t(h));
      const int32_t offset[3] = {x0 - px, y0 - py,
                                 regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                                     xenos::DepthRenderTargetFormat::kD24FS8};
      if (pl::RenderPipeline* pipeline = m.ResolvePipeline(dest_format, rt->image.samples, depth)) {
        pl::RenderDescriptorSet* set = m.resolve_pool.Next(m.device, m.context_index);
        set->setTexture(0, rt->image.texture.get(), pl::RenderTextureLayout::SHADER_READ);
        set->setSampler(1, m.point_sampler.get());
        m.DrawResolve(image, *rt, pipeline, m.resolve_layout.get(), set, scissor, offset, 8);
      } else {
        result.note = "no resolve pipeline";
      }
      if (packed.texture) {
        if (pl::RenderPipeline* pipeline = m.PackPipeline(rt->image.samples)) {
          pl::RenderDescriptorSet* set = m.pack_pool.Next(m.device, m.context_index);
          set->setTexture(0, rt->image.texture.get(), pl::RenderTextureLayout::SHADER_READ);
          set->setTexture(1, rt->image.texture.get(), pl::RenderTextureLayout::SHADER_READ,
                          m.View(rt->image, kViewStencil, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA));
          set->setSampler(2, m.point_sampler.get());
          m.DrawResolve(packed, *rt, pipeline, m.pack_layout.get(), set, scissor, offset, 12);
          m.Transition(list, packed, pl::RenderTextureLayout::SHADER_READ);
        } else {
          result.note = "no depth packing pipeline";
        }
      }
    }
    // Readback of the whole destination texture, comparable with the
    // emulator's copy of it in guest memory.
    Readback rb;
    rb.format = image.format;
    if (m.readback && FormatBytes(rb.format)) {
      rb.buffer = m.device->createBuffer(
          pl::RenderBufferDesc::ReadbackBuffer(uint64_t(tw) * th * FormatBytes(rb.format)));
    }
    if (rb.buffer) {
      m.Transition(list, image, pl::RenderTextureLayout::COPY_SOURCE);
      list.copyTextureRegion(
          pl::RenderTextureCopyLocation::PlacedFootprint(rb.buffer.get(), ToRenderFormat(rb.format),
                                                         tw, th, 1, tw),
          pl::RenderTextureCopyLocation::Subresource(image.texture.get()));
      result.width = tw, result.height = th;
      if (depth) result.note = "depth";
      rb.result = result;
      m.readbacks.push_back(std::move(rb));
      result.note.clear();
    }
    // Small float resolves: checked for inf / NaN after the frame.
    if (!m.readback && uint64_t(tw) * th <= 64 && FormatBytes(image.format) &&
        (image.format == VK_FORMAT_R16G16_SFLOAT || image.format == VK_FORMAT_R16_SFLOAT ||
         image.format == VK_FORMAT_R16G16B16A16_SFLOAT || image.format == VK_FORMAT_R32_SFLOAT ||
         image.format == VK_FORMAT_R32G32_SFLOAT)) {
      Readback check;
      check.format = image.format;
      check.buffer = m.device->createBuffer(
          pl::RenderBufferDesc::ReadbackBuffer(uint64_t(tw) * th * FormatBytes(image.format)));
      if (check.buffer) {
        m.Transition(list, image, pl::RenderTextureLayout::COPY_SOURCE);
        list.copyTextureRegion(
            pl::RenderTextureCopyLocation::PlacedFootprint(
                check.buffer.get(), ToRenderFormat(image.format), tw, th, 1, tw),
            pl::RenderTextureCopyLocation::Subresource(image.texture.get()));
        check.result.index = index;
        check.result.dest_base = dest.base_address << 12;
        check.result.width = tw, check.result.height = th;
        m.Context().checks.push_back(std::move(check));
      }
    }
    // Stage probe: the middle 8x8 of a color resolve of at least 64x64.
    if (Impl::FrameContext& c = m.Context();
        !m.readback && !depth && tw >= 64 && th >= 64 && FormatBytes(image.format) &&
        FormatBytes(image.format) <= 8 && c.stage_probes.size() < Impl::kStageProbes) {
      if (!c.stage_probe)
        c.stage_probe = m.device->createBuffer(pl::RenderBufferDesc::ReadbackBuffer(
            Impl::kStageProbes * Impl::kStageProbeBytes));
      if (c.stage_probe) {
        m.Transition(list, image, pl::RenderTextureLayout::COPY_SOURCE);
        const int32_t x = int32_t(tw / 2 - 4), y = int32_t(th / 2 - 4);
        const pl::RenderBox box(x, y, x + 8, y + 8);
        list.copyTextureRegion(
            pl::RenderTextureCopyLocation::PlacedFootprint(
                c.stage_probe.get(), ToRenderFormat(image.format), 8, 8, 1, 8,
                c.stage_probes.size() * Impl::kStageProbeBytes),
            pl::RenderTextureCopyLocation::Subresource(image.texture.get()), 0, 0, 0, &box);
        c.stage_probes.push_back({index, uint32_t(dest.base_address) << 12, tw, th, image.format});
      }
    }
    m.Transition(list, image, pl::RenderTextureLayout::SHADER_READ);
  }
  if (!result.note.empty() && m.readback) {
    Readback rb;
    rb.result = result;
    m.readbacks.push_back(std::move(rb));
  }
  m.frame_resolves += fmt::format(" {}:{:08X}/{}x{}{}", index, dest.base_address << 12, dw, dh,
                                  depth ? "d" : "");
  if (!m.debug_dir.empty())
    m.frame_log.push_back(fmt::format(
        "resolve {} flags {:X} src {}{:X}/fmt{}/{}x{}/{}x -> {:08X}/f{} {}x{} rect {},{}-{},{} at "
        "{},{}{}",
        index, flags, rt ? (rt->depth ? "D" : "C") : "-", rt ? rt->base : 0, rt ? rt->format : 0,
        rt ? rt->image.width : 0, rt ? rt->image.height : 0, rt ? rt->samples : 0,
        dest.base_address << 12, uint32_t(dest.format), dw, dh, x0, y0, x1, y1, px, py,
        result.note.empty() ? "" : " note: " + result.note));
  if (Impl::FrameContext& c = m.Context();
      c.timestamps && m.timestamp_period_ns > 0 && c.resolve_stamps < kResolveStamps)
    m.draw_list->writeTimestamp(c.timestamps.get(), 2 + c.resolve_stamps++);
  // Clears after the copy (D3DRESOLVE_CLEARRENDERTARGET / CLEARDEPTHSTENCIL),
  // of the resolved rectangle only: with predicated tiling every tile is
  // resolved and cleared on its own, while the native renderer has drawn all of
  // them at once.
  const pl::RenderRect area(x0, y0, std::max(x1, x0), std::max(y1, y0));
  if (flags & 0x100) {
    if (RenderTarget* target = m.GetRenderTarget(regs, int(flags & 3))) {
      float c[4] = {};
      if (clear_color) std::memcpy(c, clear_color, sizeof(c));
      m.ClearRegion(*target, rect ? &area : nullptr, c, 0.0f, 0, false, false);
    }
  }
  if (flags & 0x200) {
    if (RenderTarget* target = m.GetRenderTarget(regs, -1)) {
      const float c[4] = {};
      m.ClearRegion(*target, rect ? &area : nullptr, c, clear_depth, clear_stencil, true, true);
    }
  }
}

// Draws the front buffer through the gamma ramp into the presenter's guest
// output image, left in the layout the presenter samples it in.
void Renderer::Impl::RecordPresent(VkImage target, bool target_read_before, TextureEntry& source,
                                   const uint32_t* gamma_ramp) {
  FrameContext& c = Context();
  const uint32_t width = source.image.width, height = source.image.height;
  auto output = WrapImage(*plume, target, VK_FORMAT_A2B10G10R10_UNORM_PACK32, width, height,
                          target_read_before);
  if (!output) return Note("present: guest output not usable");
  const pl::RenderTexture* attachment = output.get();
  auto framebuffer = device->createFramebuffer(pl::RenderFramebufferDesc(&attachment, 1));
  if (!framebuffer) return Note("present: no framebuffer");

  // The ramp, as 64 uvec4 (std140).
  uint64_t ramp_offset;
  auto* ramp = reinterpret_cast<uint32_t*>(
      Allocate(*uniform_arena, 1024, dev->properties().minUniformBufferOffsetAlignment, ramp_offset));
  if (!ramp) return Note("present: uniform arena full");
  bool any = false;
  for (uint32_t i = 0; gamma_ramp && i < 256; ++i) any |= gamma_ramp[i] != 0;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = i * 1023 / 255;  // identity when the guest has set none
    ramp[i] = any ? gamma_ramp[i] : (v << 20 | v << 10 | v);
  }
  pl::RenderDescriptorSet* set = present_pool.Next(device, context_index);
  set->setTexture(0, source.image.texture.get(), pl::RenderTextureLayout::SHADER_READ);
  const pl::RenderBufferStructuredView at(1, uint32_t(ramp_offset));
  set->setBuffer(1, uniform_arena->buffer.get(), 1024, &at);
  set->setSampler(2, point_sampler.get());

  pl::RenderCommandList& list = *draw_list;
  const pl::RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
  const pl::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
  auto fullscreen = [&](pl::RenderFramebuffer* target, pl::RenderPipelineLayout* layout,
                        pl::RenderPipeline* pipeline, pl::RenderDescriptorSet* pass_set) {
    list.setFramebuffer(target);
    list.setGraphicsPipelineLayout(layout);
    list.setPipeline(pipeline);
    list.setGraphicsDescriptorSet(pass_set, 0);
    list.setViewports(&viewport, 1);
    list.setScissors(&scissor, 1);
    list.drawInstanced(3, 1, 0, 0);
    list.setFramebuffer(nullptr);
  };
  Transition(list, source.image, pl::RenderTextureLayout::SHADER_READ);
  // FXAA: the gamma-corrected frame in an image of our own first, anti-aliased
  // from there into the output.
  bool fxaa_pass = false;
  if (fxaa) {
    Image& image = fxaa_image;
    if (image.texture && (image.width != width || image.height != height)) DestroyLater(image);
    if (!image.texture) {
      image.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      image.width = width, image.height = height;
      const pl::RenderTexture* fxaa_attachment = nullptr;
      if (CreateImage(image, pl::RenderTextureFlag::RENDER_TARGET)) {
        fxaa_attachment = image.texture.get();
        image.framebuffer =
            device->createFramebuffer(pl::RenderFramebufferDesc(&fxaa_attachment, 1));
      }
      if (!image.framebuffer) image = Image{};
    }
    if (image.framebuffer) {
      Transition(list, image, pl::RenderTextureLayout::COLOR_WRITE);
      fullscreen(image.framebuffer.get(), present_layout.get(), present_pipeline.get(), set);
      Transition(list, image, pl::RenderTextureLayout::SHADER_READ);
      fxaa_pass = true;
    }
  }
  list.barriers(kAllStages, pl::RenderTextureBarrier(output.get(), pl::RenderTextureLayout::COLOR_WRITE));
  if (fxaa_pass) {
    pl::RenderDescriptorSet* fxaa_set = fxaa_pool.Next(device, context_index);
    fxaa_set->setTexture(0, fxaa_image.texture.get(), pl::RenderTextureLayout::SHADER_READ);
    fxaa_set->setSampler(1, linear_sampler.get());
    fullscreen(framebuffer.get(), fxaa_layout.get(), fxaa_pipeline.get(), fxaa_set);
  } else {
    fullscreen(framebuffer.get(), present_layout.get(), present_pipeline.get(), set);
  }
  // VulkanPresenter::kGuestOutputInternalLayout.
  list.barriers(pl::RenderBarrierStage::GRAPHICS,
                pl::RenderTextureBarrier(output.get(), pl::RenderTextureLayout::SHADER_READ));
  c.output_framebuffers.push_back(std::move(framebuffer));
  c.outputs.push_back(std::move(output));
}

bool Renderer::EndFrame(const PresentRequest* present, std::vector<ResolvedImage>* resolves,
                        std::string* error) {
  Impl& m = *impl_;
  if (!m.frame_open) return false;
  m.frame_open = false;
  m.EndRendering();
  Impl::FrameContext& c = m.Context();
  bool submitted = false;
  auto submit = [&] {
    if (c.timestamps && m.timestamp_period_ns > 0) {
      // Every query of the pool written: reading them back fails (VK_NOT_READY
      // on RADV, stale results) if one of them never was.
      for (uint32_t i = c.resolve_stamps; i < kResolveStamps; ++i)
        m.draw_list->writeTimestamp(c.timestamps.get(), 2 + i);
      m.draw_list->writeTimestamp(c.timestamps.get(), 1);
    }
    m.upload_list->end();
    m.draw_list->end();
    const pl::RenderCommandList* lists[2] = {m.upload_list, m.draw_list};
    m.queue->executeCommandLists(lists, 2, nullptr, 0, nullptr, 0, c.fence.get());
    c.submitted = submitted = true;
    c.stage_probes_done = std::move(c.stage_probes);
    c.stage_probes.clear();
  };

  // The presenter hands over its guest output image for the length of the
  // callback, which must submit the commands writing it (on the queue the
  // presenter uses: the SDK's graphics queue 0, Plume's too).
  if (present && present->presenter) {
    auto it = m.resolved.find(present->frontbuffer_base >> 12);
    if (it == m.resolved.end() || !it->second.image.texture) {
      m.Note(fmt::format("present: front buffer {:08X} was not resolved", present->frontbuffer_base));
    } else {
      TextureEntry& source = it->second;
      // Black-frame detector: 16 patches of 8x8 over the middle of the frame.
      if (source.image.format == VK_FORMAT_R8G8B8A8_UNORM && source.image.width >= 64 &&
          source.image.height >= 64) {
        if (!c.dark_probe)
          c.dark_probe = m.device->createBuffer(pl::RenderBufferDesc::ReadbackBuffer(16 * 64 * 4));
        if (c.dark_probe) {
          m.Transition(*m.draw_list, source.image, pl::RenderTextureLayout::COPY_SOURCE);
          for (uint32_t i = 0; i < 16; ++i) {
            const int32_t x = int32_t(source.image.width * (2 + (i & 3) * 2) / 10);
            const int32_t y = int32_t(source.image.height * (3 + (i >> 2) * 15 / 10) / 10);
            const pl::RenderBox box(x, y, x + 8, y + 8);
            m.draw_list->copyTextureRegion(
                pl::RenderTextureCopyLocation::PlacedFootprint(
                    c.dark_probe.get(), pl::RenderFormat::R8G8B8A8_UNORM, 8, 8, 1, 8, i * 256),
                pl::RenderTextureCopyLocation::Subresource(source.image.texture.get()), 0, 0, 0,
                &box);
          }
          c.dark_probed = true;
          c.dark_frame = m.frame;
          c.dark_resolves = m.frame_resolves;
          c.dark_draws = m.stats.draws;
        }
      }
      // Debug: the patch where the giant ball showed, (800,60)-(1150,300).
      const uint32_t px = 800 * m.scale, py = 60 * m.scale, pw = 350 * m.scale, ph = 240 * m.scale;
      if (!m.debug_dir.empty() && source.image.format == VK_FORMAT_R8G8B8A8_UNORM &&
          source.image.width >= px + pw && source.image.height >= py + ph) {
        m.probe_bytes = uint64_t(pw) * ph * 4;
        if (!c.probe) c.probe = m.device->createBuffer(pl::RenderBufferDesc::ReadbackBuffer(m.probe_bytes));
        if (c.probe) {
          m.Transition(*m.draw_list, source.image, pl::RenderTextureLayout::COPY_SOURCE);
          const pl::RenderBox box(int32_t(px), int32_t(py), int32_t(px + pw), int32_t(py + ph));
          m.draw_list->copyTextureRegion(
              pl::RenderTextureCopyLocation::PlacedFootprint(c.probe.get(),
                                                             pl::RenderFormat::R8G8B8A8_UNORM, pw,
                                                             ph, 1, pw),
              pl::RenderTextureCopyLocation::Subresource(source.image.texture.get()), 0, 0, 0,
              &box);
          c.probed = true;
          c.probe_frame = m.frame;
        }
      }
      present->presenter->RefreshGuestOutput(
          source.image.width, source.image.height, present->aspect_x, present->aspect_y,
          [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
            auto& vulkan_context =
                static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
                    context);
            m.RecordPresent(vulkan_context.image(), vulkan_context.image_ever_written_previously(),
                            source, present->gamma_ramp);
            context.SetIs8bpc(true);
            submit();
            return true;
          });
    }
  }
  if (!submitted) submit();
  c.log = std::move(m.frame_log);
  m.frame_log.clear();
  if (error) error->clear();
  if (!resolves) {
    m.readbacks.clear();
    return true;
  }
  m.queue->waitForCommandFence(c.fence.get());
  c.submitted = false;
  for (Readback& rb : m.readbacks) {
    ResolvedImage& out = resolves->emplace_back(std::move(rb.result));
    if (!rb.buffer) continue;
    const auto* mapping = static_cast<const uint8_t*>(rb.buffer->map());
    if (!mapping) {
      out.note = "readback failed";
      continue;
    }
    const uint32_t texel = FormatBytes(rb.format);
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (size_t i = 0; i < size_t(out.width) * out.height; ++i)
      TexelToRgba8(rb.format, mapping + i * texel, &out.rgba[i * 4]);
    if (rb.format == VK_FORMAT_R16G16B16A16_SFLOAT)
      out.rgba16f.assign(mapping, mapping + size_t(out.width) * out.height * 8);
    rb.buffer->unmap();
  }
  m.readbacks.clear();
  return true;
}

void Renderer::WatchGuestMemory(rex::memory::Memory* memory) { PageWatch::Get().Attach(memory); }

void Renderer::WaitIdle() {
  Impl& m = *impl_;
  for (Impl::FrameContext& c : m.contexts)
    if (c.submitted) {
      m.queue->waitForCommandFence(c.fence.get());
      c.submitted = false;
    }
}

void Renderer::SetKeepResolves(bool keep) { impl_->keep_resolves = keep; }

void Renderer::SetSkipDraws(uint32_t first, uint32_t end) {
  impl_->skip_first = first, impl_->skip_end = end;
}

uint32_t Renderer::resolution_scale() const { return impl_->scale; }

void Renderer::SetDebugFrames(const std::string& dir) { impl_->debug_dir = dir; }

void Renderer::SetMsaaSamples(uint32_t samples) { impl_->msaa_override = samples; }

void Renderer::SetFxaa(bool fxaa) { impl_->fxaa = fxaa; }

}  // namespace tb2::native
