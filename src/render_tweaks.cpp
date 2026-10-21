// thebigs2 - main scene MSAA / predicated tiling; the depth of field NaN fix.
//
// At startup the game (sub_82219148) sets up its main scene render pass
// (engine render-pass table entry 5) with 4x MSAA. A 1280x720 frame at 4x MSAA
// doesn't fit the Xbox 360's 10 MB EDRAM, so it also enables predicated tiling
// with 3 tiles (240-row bands): every frame the GPU replays the scene's
// command buffer once per tile. Emulated, that triples the command processor's
// work for the scene (about 3,800 draws a frame) and renders 4x MSAA. Without
// MSAA, 1280x720 colour + depth (7.4 MB) fits in EDRAM in one piece, so the
// game needs no tiling at all.
//
// Engine setters (renderlib/xbox360), entry = this->table(+3616) + index * 348:
//   sub_82491BF8(this, index, count): +264 tile count, +20 tiling on (count != 0)
//   sub_82491CB8(this, index, type):  +268 multisample type (0 none, 1 2x, 2 4x)
//   sub_82491C20(this, index, tile, ...): tile rectangles (unused without tiling)

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(scene_msaa, true, "The Bigs 2",
                    "Main scene 4x MSAA as on the Xbox 360. false = no MSAA, which also "
                    "removes the 3-tile rendering the console needed for it (much less CPU "
                    "and GPU work; edges are less smooth at 1x)");

extern "C" REX_FUNC(__imp__sub_82491BF8);
extern "C" REX_FUNC(sub_82491BF8) {
  if (!REXCVAR_GET(scene_msaa) && ctx.r5.u32 > 1) {
    REXLOG_INFO("[render] entry {}: predicated tiling ({} tiles) off", ctx.r4.u32, ctx.r5.u32);
    ctx.r5.u64 = 0;
  }
  [[clang::musttail]] return __imp__sub_82491BF8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82491CB8);
extern "C" REX_FUNC(sub_82491CB8) {
  if (!REXCVAR_GET(scene_msaa) && ctx.r5.u32 != 0) {
    REXLOG_INFO("[render] entry {}: multisample type {} -> none", ctx.r4.u32, ctx.r5.u32);
    ctx.r5.u64 = 0;
  }
  [[clang::musttail]] return __imp__sub_82491CB8(ctx, base);
}

// Depth of field blur at a camera cut (the strike-out cameras): on the
// effect's first frame the game's blur radius is 0 and its 16 Gaussian
// weights come out 0/0 = NaN (pixel shader c65-c68, sample offsets c69-c76
// all 0). Both blur passes then write 0 and the frame's composite comes out
// black. A zero-radius blur is a copy: equal weights over the 16 taps (at
// offset 0) make it one. SetPixelShaderConstantF = sub_823BC8E8 (r3 device,
// r4 first register, r5 data, r6 register count); the data is the engine's
// per-frame material constant block (sub_82492B08).
extern "C" REX_FUNC(__imp__sub_823BC8E8);
extern "C" REX_FUNC(sub_823BC8E8) {
  if (ctx.r4.u32 == 65 && ctx.r6.u32 == 4) {
    auto at = [&](uint32_t i) {
      const uint32_t a = ctx.r5.u32 + i * 4;
#if defined(_WIN32)
      return reinterpret_cast<uint8_t*>(base) + a + (a >= 0xE0000000u ? 0x1000u : 0u);
#else
      return reinterpret_cast<uint8_t*>(base) + a;
#endif
    };
    bool all_nan = true;
    for (uint32_t i = 0; i < 16 && all_nan; ++i) {
      uint32_t v;
      std::memcpy(&v, at(i), 4);
      v = __builtin_bswap32(v);
      all_nan = (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu);
    }
    if (all_nan) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 5)
        REXLOG_INFO("[render] depth of field blur with NaN weights (radius 0): made a copy");
      const uint32_t sixteenth = __builtin_bswap32(0x3D800000u);  // 1/16
      for (uint32_t i = 0; i < 16; ++i) std::memcpy(at(i), &sixteenth, 4);
    }
  }
  [[clang::musttail]] return __imp__sub_823BC8E8(ctx, base);
}

// Which EDRAM emulation path the GPU emulation uses, logged at startup: the
// SDK falls back from "fsi" to host render targets without saying so on GPUs
// without fragment shader interlock (and the related features its FSI path
// needs - the same check as VulkanRenderTargetCache::Initialize).
void TB2_LogRenderTargetPath() {
  auto* graphics = rex::system::kernel_state()->emulator()->graphics_system();
  auto* provider =
      graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
  if (!provider) return;  // not Vulkan
  const auto& p = provider->vulkan_device()->properties();
  const bool interlock = p.fragmentShaderSampleInterlock || p.fragmentShaderPixelInterlock;
  const bool fsi_supported =
      interlock && p.fragmentStoresAndAtomics && p.sampleRateShading && p.standardSampleLocations;
  const std::string requested = rex::cvar::GetFlagByName("render_target_path_vulkan");
  const bool fsi = requested == "fsi" && fsi_supported;
  REXLOG_INFO("EDRAM path: {} (requested \"{}\"; {}: fragment shader interlock {}, {}){}",
              fsi ? "fsi" : "host render targets", requested, p.deviceName,
              interlock ? "yes" : "no", fsi_supported ? "FSI path supported" : "FSI path not supported",
              requested == "fsi" && !fsi_supported
                  ? " - consider resolution_scale = 1 and render_target_path_vulkan = \"host\""
                  : "");
}
