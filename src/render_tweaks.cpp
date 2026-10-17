// thebigs2 - main scene MSAA / predicated tiling.
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

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "render_trace.h"

REXCVAR_DEFINE_BOOL(scene_msaa, true, "The Bigs 2",
                    "Main scene 4x MSAA as on the Xbox 360. false = no MSAA, which also "
                    "removes the 3-tile rendering the console needed for it (much less CPU "
                    "and GPU work; edges are less smooth at 1x)");

extern "C" REX_FUNC(__imp__sub_82491BF8);
extern "C" REX_FUNC(sub_82491BF8) {
  TB2_TRACE_HOOKED(0x82491BF8);
  if (!REXCVAR_GET(scene_msaa) && ctx.r5.u32 > 1) {
    REXLOG_INFO("[render] entry {}: predicated tiling ({} tiles) off", ctx.r4.u32, ctx.r5.u32);
    ctx.r5.u64 = 0;
  }
  [[clang::musttail]] return __imp__sub_82491BF8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82491CB8);
extern "C" REX_FUNC(sub_82491CB8) {
  TB2_TRACE_HOOKED(0x82491CB8);
  if (!REXCVAR_GET(scene_msaa) && ctx.r5.u32 != 0) {
    REXLOG_INFO("[render] entry {}: multisample type {} -> none", ctx.r4.u32, ctx.r5.u32);
    ctx.r5.u64 = 0;
  }
  [[clang::musttail]] return __imp__sub_82491CB8(ctx, base);
}
