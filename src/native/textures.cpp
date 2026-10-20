// thebigs2 - native renderer: Xenos textures -> host textures (see textures.h).

#include "textures.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>

namespace tb2::native {

namespace xg = rex::graphics;
using xg::xenos::TextureFormat;

namespace {

enum class Conversion { kCopy, kCtx1ToR8G8, kDepthUnorm24, kDepthFloat24 };

struct FormatMapping {
  VkFormat format;
  Conversion conversion;
};

// Following the SDK's Vulkan texture cache: data stays unsigned / unnormalized
// as in memory, signedness and gamma are applied when sampling.
FormatMapping Mapping(TextureFormat format) {
  switch (format) {
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
    case TextureFormat::k_8_B:
      return {VK_FORMAT_R8_UNORM, Conversion::kCopy};
    case TextureFormat::k_8_8:
      return {VK_FORMAT_R8G8_UNORM, Conversion::kCopy};
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
    case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return {VK_FORMAT_R8G8B8A8_UNORM, Conversion::kCopy};
    case TextureFormat::k_2_10_10_10:
    case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, Conversion::kCopy};
    case TextureFormat::k_DXT1:
    case TextureFormat::k_DXT1_AS_16_16_16_16:
      return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, Conversion::kCopy};
    case TextureFormat::k_DXT2_3:
    case TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return {VK_FORMAT_BC2_UNORM_BLOCK, Conversion::kCopy};
    case TextureFormat::k_DXT4_5:
    case TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return {VK_FORMAT_BC3_UNORM_BLOCK, Conversion::kCopy};
    case TextureFormat::k_DXN:
      return {VK_FORMAT_BC5_UNORM_BLOCK, Conversion::kCopy};
    case TextureFormat::k_DXT5A:
      return {VK_FORMAT_BC4_UNORM_BLOCK, Conversion::kCopy};
    case TextureFormat::k_CTX1:
      return {VK_FORMAT_R8G8_UNORM, Conversion::kCtx1ToR8G8};
    case TextureFormat::k_16:
      return {VK_FORMAT_R16_UNORM, Conversion::kCopy};
    case TextureFormat::k_16_16:
      return {VK_FORMAT_R16G16_UNORM, Conversion::kCopy};
    case TextureFormat::k_16_16_16_16:
      return {VK_FORMAT_R16G16B16A16_UNORM, Conversion::kCopy};
    case TextureFormat::k_16_EXPAND:
    case TextureFormat::k_16_FLOAT:
      return {VK_FORMAT_R16_SFLOAT, Conversion::kCopy};
    case TextureFormat::k_16_16_EXPAND:
    case TextureFormat::k_16_16_FLOAT:
      return {VK_FORMAT_R16G16_SFLOAT, Conversion::kCopy};
    case TextureFormat::k_16_16_16_16_EXPAND:
    case TextureFormat::k_16_16_16_16_FLOAT:
      return {VK_FORMAT_R16G16B16A16_SFLOAT, Conversion::kCopy};
    case TextureFormat::k_32_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, Conversion::kCopy};
    case TextureFormat::k_32_32_FLOAT:
      return {VK_FORMAT_R32G32_SFLOAT, Conversion::kCopy};
    case TextureFormat::k_32_32_32_32_FLOAT:
      return {VK_FORMAT_R32G32B32A32_SFLOAT, Conversion::kCopy};
    // Depth (resolve targets, e.g. the shadow map): the depth as a float, like
    // the SDK (shaders sample it as a float texture; stencil is dropped).
    case TextureFormat::k_24_8:
      return {VK_FORMAT_R32_SFLOAT, Conversion::kDepthUnorm24};
    case TextureFormat::k_24_8_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, Conversion::kDepthFloat24};
    default:
      return {VK_FORMAT_UNDEFINED, Conversion::kCopy};
  }
}

}  // namespace

VkFormat HostTextureFormat(TextureFormat format) { return Mapping(format).format; }

bool ConvertTexture(const uint8_t* memory, const xg::xenos::xe_gpu_texture_fetch_t& fetch,
                    HostTexture& out, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  const FormatMapping mapping = Mapping(fetch.format);
  const xg::FormatInfo* guest_info = xg::FormatInfo::Get(fetch.format);
  if (mapping.format == VK_FORMAT_UNDEFINED || !guest_info)
    return fail(fmt::format("format {} not supported",
                            guest_info ? guest_info->name : std::to_string(uint32_t(fetch.format))));

  uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
  xg::texture_util::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1,
                                                     &base_page, &mip_page, &mip_min, &mip_max);
  if (!base_page) return fail("no base level (only mips)");
  const uint32_t width = width_m1 + 1, height = height_m1 + 1;
  const bool is_3d = fetch.dimension == xg::xenos::DataDimension::k3D;
  const uint32_t depth = is_3d ? depth_m1 + 1 : 1;
  const uint32_t layers = is_3d ? 1 : depth_m1 + 1;

  const xg::texture_util::TextureGuestLayout layout = xg::texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width, height, depth_m1 + 1, fetch.tiled, fetch.format,
      fetch.packed_mips, true, mip_max);

  out = HostTexture{};
  out.format = mapping.format;
  switch (fetch.dimension) {
    case xg::xenos::DataDimension::k1D: out.view_type = VK_IMAGE_VIEW_TYPE_1D; break;
    case xg::xenos::DataDimension::k2DOrStacked:
      out.view_type = fetch.stacked ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
      break;
    case xg::xenos::DataDimension::k3D: out.view_type = VK_IMAGE_VIEW_TYPE_3D; break;
    case xg::xenos::DataDimension::kCube: out.view_type = VK_IMAGE_VIEW_TYPE_CUBE; break;
  }
  out.width = width, out.height = height, out.depth = depth, out.layers = layers;
  const uint32_t gbw = guest_info->block_width, gbh = guest_info->block_height;
  const uint32_t guest_bpb = guest_info->bytes_per_block();
  const uint32_t guest_bpb_log2 = std::countr_zero(guest_bpb);
  // Host blocks: CTX1 becomes R8G8 texels, everything else keeps its blocks.
  const bool ctx1 = mapping.conversion == Conversion::kCtx1ToR8G8;
  out.block_width = ctx1 ? 1 : gbw;
  out.block_height = ctx1 ? 1 : gbh;
  out.bytes_per_block = ctx1 ? 2 : guest_bpb;

  // Host layout: level by level, layers within a level, depth slices within a
  // layer. CTX1 rows are padded to whole 4x4 guest blocks.
  size_t total = 0;
  for (uint32_t level = 0; level <= mip_max; ++level) {
    HostTexture::Level l;
    l.width = std::max(width >> level, 1u);
    l.height = std::max(height >> level, 1u);
    l.depth = std::max(depth >> level, 1u);
    const uint32_t blocks_x = (l.width + gbw - 1) / gbw, blocks_y = (l.height + gbh - 1) / gbh;
    l.row_pitch = ctx1 ? blocks_x * gbw * 2 : blocks_x * guest_bpb;
    l.block_rows = ctx1 ? blocks_y * gbh : blocks_y;
    l.layer_bytes = size_t(l.row_pitch) * l.block_rows * l.depth;
    l.offset = total;
    total += l.layer_bytes * layers;
    out.levels.push_back(l);
  }
  out.data.resize(total);

  for (uint32_t level = 0; level <= mip_max; ++level) {
    const HostTexture::Level& l = out.levels[level];
    // Where the level is stored: the base at the base address, mips at the mip
    // address; levels from packed_level on share the packed mip tail.
    const bool packed = level >= layout.packed_level;
    const uint32_t stored = packed ? layout.packed_level : level;
    const auto& guest_level = level == 0 ? layout.base : layout.mips[stored];
    uint32_t level_address =
        level == 0 ? base_page << 12 : (mip_page << 12) + layout.mip_offsets_bytes[stored];
    if (level != 0 && stored == 0) level_address = mip_page << 12;  // tail at level 0
    uint32_t off_x = 0, off_y = 0, off_z = 0;
    if (packed)
      xg::texture_util::GetPackedMipOffset(width, height, depth, fetch.format, level, off_x, off_y,
                                           off_z);
    const uint32_t extent = guest_level.level_data_extent_bytes;
    if (uint64_t(level_address) + extent > 0x20000000)
      return fail(fmt::format("level {} at {:08X}+{} outside physical memory", level,
                              level_address, extent));
    const uint8_t* src = memory + 0xA0000000u + level_address;
    const uint32_t pitch_blocks = guest_level.row_pitch_bytes / guest_bpb;
    const uint32_t blocks_x = (l.width + gbw - 1) / gbw, blocks_y = (l.height + gbh - 1) / gbh;

    for (uint32_t layer = 0; layer < layers; ++layer) {
      const uint8_t* layer_src = src + size_t(layer) * guest_level.array_slice_stride_bytes;
      uint8_t* layer_dst = out.data.data() + l.offset + layer * l.layer_bytes;
      for (uint32_t z = 0; z < l.depth; ++z) {
        uint8_t* slice_dst = layer_dst + size_t(z) * l.row_pitch * l.block_rows;
        for (uint32_t by = 0; by < blocks_y; ++by) {
          for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const int32_t gx = int32_t(bx + off_x), gy = int32_t(by + off_y),
                          gz = int32_t(z + off_z);
            size_t offset;
            if (!fetch.tiled) {
              offset = (size_t(gz) * guest_level.z_slice_stride_block_rows + gy) *
                           guest_level.row_pitch_bytes +
                       size_t(gx) * guest_bpb;
            } else if (is_3d) {
              offset = size_t(xg::texture_util::GetTiledOffset3D(
                  gx, gy, gz, pitch_blocks, guest_level.z_slice_stride_block_rows, guest_bpb_log2));
            } else {
              offset = size_t(
                  xg::texture_util::GetTiledOffset2D(gx, gy, pitch_blocks, guest_bpb_log2));
            }
            if (offset + guest_bpb > guest_level.array_slice_stride_bytes &&
                offset + guest_bpb > extent)
              return fail(fmt::format("level {} block ({},{}) at {} beyond the level ({} bytes)",
                                      level, gx, gy, offset, extent));
            if (ctx1) {
              xg::texture_conversion::ConvertTexelCTX1ToR8G8(
                  fetch.endianness, slice_dst + size_t(by) * 4 * l.row_pitch + bx * 8,
                  layer_src + offset, l.row_pitch);
            } else {
              uint8_t* dst = slice_dst + size_t(by) * l.row_pitch + bx * guest_bpb;
              xg::texture_conversion::CopySwapBlock(fetch.endianness, dst, layer_src + offset,
                                                    guest_bpb);
              if (mapping.conversion == Conversion::kDepthUnorm24 ||
                  mapping.conversion == Conversion::kDepthFloat24) {
                uint32_t v;
                std::memcpy(&v, dst, 4);
                const float f = mapping.conversion == Conversion::kDepthUnorm24
                                    ? float(v >> 8) * (1.0f / 16777215.0f)
                                    : xg::xenos::Float20e4To32(v >> 8);
                std::memcpy(dst, &f, 4);
              }
            }
          }
        }
      }
    }
  }
  return true;
}

// --- Debug previews --------------------------------------------------------

namespace {

void Rgb565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

// BC1 color block -> 16 RGBA texels (alpha from the 3-color mode).
void DecodeBc1Color(const uint8_t* b, uint8_t out[16][4], bool bc1_alpha) {
  const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t p[4][4] = {};
  Rgb565(c0, p[0]);
  Rgb565(c1, p[1]);
  p[0][3] = p[1][3] = 255;
  if (c0 > c1 || !bc1_alpha) {
    for (int i = 0; i < 3; ++i) {
      p[2][i] = uint8_t((2 * p[0][i] + p[1][i]) / 3);
      p[3][i] = uint8_t((p[0][i] + 2 * p[1][i]) / 3);
    }
    p[2][3] = p[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) p[2][i] = uint8_t((p[0][i] + p[1][i]) / 2);
    p[2][3] = 255;
    p[3][3] = 0;
  }
  const uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
  for (int t = 0; t < 16; ++t) std::memcpy(out[t], p[(bits >> (t * 2)) & 3], 4);
}

// BC4 / BC3-alpha block -> 16 values.
void DecodeBc4(const uint8_t* b, uint8_t out[16]) {
  uint8_t v[8] = {b[0], b[1]};
  if (b[0] > b[1]) {
    for (int i = 1; i < 7; ++i) v[i + 1] = uint8_t(((7 - i) * b[0] + i * b[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) v[i + 1] = uint8_t(((5 - i) * b[0] + i * b[1]) / 5);
    v[6] = 0, v[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = v[(bits >> (t * 3)) & 7];
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  float f;
  if (exp == 0) f = std::ldexp(float(mant), -24);
  else if (exp == 31) f = mant ? NAN : INFINITY;
  else f = std::ldexp(float(mant | 1024), int(exp) - 25);
  return sign ? -f : f;
}

uint8_t Unorm8(float f) {
  if (!(f > 0.f)) return 0;
  return f >= 1.f ? 255 : uint8_t(f * 255.f + .5f);
}

}  // namespace

bool DecodeToRgba8(const HostTexture& t, uint32_t level, uint32_t layer,
                   std::vector<uint8_t>& rgba) {
  if (level >= t.levels.size() || layer >= t.layers) return false;
  const HostTexture::Level& l = t.levels[level];
  const uint8_t* src = t.data.data() + l.offset + layer * l.layer_bytes;  // depth slice 0
  rgba.assign(size_t(l.width) * l.height * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
    if (x < l.width && y < l.height) std::memcpy(&rgba[(size_t(y) * l.width + x) * 4], c, 4);
  };
  const uint32_t blocks_x = (l.width + t.block_width - 1) / t.block_width;
  for (uint32_t by = 0; by < l.block_rows; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* b = src + size_t(by) * l.row_pitch + size_t(bx) * t.bytes_per_block;
      uint8_t texels[16][4] = {};
      bool block = true;
      switch (t.format) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: DecodeBc1Color(b, texels, true); break;
        case VK_FORMAT_BC2_UNORM_BLOCK:
          DecodeBc1Color(b + 8, texels, false);
          for (int i = 0; i < 16; ++i) texels[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
          break;
        case VK_FORMAT_BC3_UNORM_BLOCK: {
          uint8_t a[16];
          DecodeBc4(b, a);
          DecodeBc1Color(b + 8, texels, false);
          for (int i = 0; i < 16; ++i) texels[i][3] = a[i];
          break;
        }
        case VK_FORMAT_BC4_UNORM_BLOCK: {
          uint8_t r[16];
          DecodeBc4(b, r);
          for (int i = 0; i < 16; ++i) texels[i][0] = texels[i][1] = texels[i][2] = r[i], texels[i][3] = 255;
          break;
        }
        case VK_FORMAT_BC5_UNORM_BLOCK: {
          uint8_t r[16], g[16];
          DecodeBc4(b, r);
          DecodeBc4(b + 8, g);
          for (int i = 0; i < 16; ++i) texels[i][0] = r[i], texels[i][1] = g[i], texels[i][2] = 0, texels[i][3] = 255;
          break;
        }
        default: block = false;
      }
      if (block) {
        for (int i = 0; i < 16; ++i) put(bx * 4 + i % 4, by * 4 + i / 4, texels[i]);
        continue;
      }
      uint8_t c[4] = {0, 0, 0, 255};
      switch (t.format) {
        case VK_FORMAT_R8_UNORM: c[0] = c[1] = c[2] = b[0]; break;
        case VK_FORMAT_R8G8_UNORM: c[0] = b[0], c[1] = b[1]; break;
        case VK_FORMAT_R8G8B8A8_UNORM: std::memcpy(c, b, 4); break;
        case VK_FORMAT_R16_UNORM: c[0] = c[1] = c[2] = b[1]; break;
        case VK_FORMAT_R16G16_UNORM: c[0] = b[1], c[1] = b[3]; break;
        case VK_FORMAT_R16G16B16A16_UNORM: c[0] = b[1], c[1] = b[3], c[2] = b[5], c[3] = b[7]; break;
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
          const uint32_t n = t.bytes_per_block / 2;
          for (uint32_t i = 0; i < n; ++i) {
            uint16_t h;
            std::memcpy(&h, b + i * 2, 2);
            c[i] = Unorm8(HalfToFloat(h));
          }
          if (n == 1) c[1] = c[2] = c[0];
          break;
        }
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT: {
          const uint32_t n = t.bytes_per_block / 4;
          for (uint32_t i = 0; i < n; ++i) {
            float f;
            std::memcpy(&f, b + i * 4, 4);
            c[i] = Unorm8(f);
          }
          if (n == 1) c[1] = c[2] = c[0];
          break;
        }
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
          uint32_t v;
          std::memcpy(&v, b, 4);
          c[0] = uint8_t((v & 1023) >> 2), c[1] = uint8_t(((v >> 10) & 1023) >> 2);
          c[2] = uint8_t(((v >> 20) & 1023) >> 2), c[3] = uint8_t((v >> 30) * 85);
          break;
        }
        default: return false;
      }
      put(bx, by, c);
    }
  }
  return true;
}

}  // namespace tb2::native
