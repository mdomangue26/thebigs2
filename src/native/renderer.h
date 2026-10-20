// thebigs2 - native renderer: draws the game's D3D draws through Plume
// (docs/native-renderer.md, phases 3-5).
//
// Fed per draw with the GPU state the D3D device shadow holds (as a register
// file), the draw's shader microcode and its draw packet. Renders into host
// render targets of its own (one texture per EDRAM surface: no EDRAM
// emulation) on the SDK's Vulkan device through Plume (plume_device.h);
// resolves draw the render target into a texture later draws can sample (kept
// across frames). Commands are recorded during the frame and submitted at its
// end, two frames in flight.
//
// Live (phase 5) it presents each frame's front buffer through the SDK
// presenter with the guest's gamma ramp; for captures every resolve is read
// back for comparison with the emulator.
//
// MSAA targets use native sample counts (resolves average color and take
// depth sample 0); predicated tiling is drawn once and resolved / cleared per
// tile. Not handled: memexport, EDRAM aliasing between differently shaped
// surfaces, the 7e3 precision and 2-bit alpha of 2_10_10_10_FLOAT targets
// (half floats, like the SDK's host render target path), line polygon mode,
// gaps between bound color targets.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/register_file.h>

#include "buffers.h"

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace tb2::native {

// A resolve's destination texture, read back at the end of the frame (RGBA8).
struct ResolvedImage {
  uint32_t index = 0;      // resolve number in the frame
  uint32_t dest_base = 0;  // physical address of the destination texture
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  // Half-float render targets: the texels as they are (RGBA16F), since the
  // RGBA8 image clips values above 1.
  std::vector<uint8_t> rgba16f;
  std::string note;  // why it is missing or approximate
};

struct RendererStats {
  uint32_t draws = 0, draws_skipped = 0, clears = 0, resolves = 0;
  uint32_t pipelines_created = 0, textures_uploaded = 0, texture_failures = 0;
  uint32_t textures_from_resolves = 0, render_passes = 0, signed_texture_bindings = 0;
  uint32_t resolves_overwritten = 0;  // resolved textures dropped: the CPU wrote their memory
  uint64_t vertex_bytes = 0, uniform_bytes = 0, upload_bytes = 0;
  float gpu_ms_previous = 0;  // GPU time of the frame this one's resources were last used by
  std::vector<std::string> notes;  // first few problems (skipped draws, ...)
};

// What EndFrame presents: the texture the guest's Swap shows.
struct PresentRequest {
  rex::ui::Presenter* presenter = nullptr;  // the SDK's
  uint32_t frontbuffer_base = 0;            // physical address of the front buffer texture
  uint32_t aspect_x = 16, aspect_y = 9;     // display aspect ratio
  const uint32_t* gamma_ramp = nullptr;     // 256 DC_LUT_30_COLOR entries; null = identity
};

class Renderer {
 public:
  // On the SDK's Vulkan device, which must outlive the renderer. Null with a
  // reason if it isn't usable.
  static std::unique_ptr<Renderer> Create(const rex::ui::vulkan::VulkanDevice* device,
                                          std::string* error);
  ~Renderer();

  // memory: guest memory base (physical address p at memory + 0xA0000000 + p).
  // readback: read every resolve back at the end of the frame (captures).
  // continuous: the previous frame was drawn too (live), so its resolved
  // textures still stand for their memory; otherwise they are dropped.
  void BeginFrame(const uint8_t* memory, bool readback, bool continuous);

  // A draw with the state in regs (VGT_DRAW_INITIATOR, VGT_DMA_* included in
  // draw). ucode: big-endian microcode of the vertex and pixel shader.
  void Draw(const rex::graphics::RegisterFile& regs, std::span<const uint32_t> vs_ucode,
            std::span<const uint32_t> ps_ucode, const GuestDraw& draw);

  // D3D Clear: flags bits 0-3 color targets, 0x10 depth, 0x20 stencil; color
  // as D3DCOLOR (ARGB). bound: the surfaces D3D has set (bits 0-3 render
  // targets, bit 4 depth / stencil).
  void Clear(const rex::graphics::RegisterFile& regs, uint32_t flags, uint32_t color, float depth,
             uint32_t stencil, uint32_t bound);

  // D3D Resolve: flags bits 0-1 render target, bit 2 depth (copied as float),
  // bits 4-6 the MSAA sample (ignored: color is averaged, depth takes sample
  // 0), 0x100 / 0x200 clear color / depth afterwards. rect: source rectangle
  // (x0, y0, x1, y1) or null for the destination's size; dest_point: where it
  // goes in the destination (null: 0, 0). dest_fetch: the destination
  // texture's fetch constant (host byte order). With readback, the whole
  // destination texture is read back.
  void Resolve(const rex::graphics::RegisterFile& regs, uint32_t flags, const int32_t* rect,
               const int32_t* dest_point, const uint32_t dest_fetch[6],
               const float clear_color[4], float clear_depth, uint32_t clear_stencil);

  // Submits the frame, presenting it if asked. With readback, waits for it and
  // appends the resolves; otherwise returns at once.
  bool EndFrame(const PresentRequest* present, std::vector<ResolvedImage>* resolves,
                std::string* error);

  // Tracks CPU writes to guest memory, so unchanged textures and vertex
  // buffers aren't hashed again every frame (SDK write watches).
  void WatchGuestMemory(rex::memory::Memory* memory);
  // Waits for the frames in flight.
  void WaitIdle();
  // Keep resolved textures even when their guest memory changes (comparisons
  // with readback_resolve, where the emulator writes its resolves there).
  void SetKeepResolves(bool keep);

  const RendererStats& stats() const;
  std::string DeviceName() const;

 private:
  Renderer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tb2::native
