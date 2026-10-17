# Native Vulkan renderer — plan and findings

Status: **Phases 0 (mapping) and 1 (shadow capture) done; phase 2 (resources)
next.** Work happens on the
`native-renderer` branch. The game still renders through the SDK's Xenos GPU
emulator; nothing here changes that yet. Findings: [d3d-map.md](d3d-map.md).

## Why

The current path emulates the Xbox 360 GPU: the game's D3D library writes PM4
command packets into a ring buffer, and the Xenos plugin decodes them, emulates
EDRAM render targets and translates shaders at run time. It works, but:

- EDRAM emulation is the source of the 2xMSAA/scaling bugs (green bat at 2x on
  the host-render-target path) and needs fragment shader interlock to be
  correct, which AMD RADV / Steam Deck lack.
- Every frame pays for packet decoding, render-target ownership tracking and
  resolve emulation that a PC GPU doesn't need.
- Things a port should have — widescreen/ultrawide, real MSAA or better AA,
  resolution independence, proper frame pacing — are hard to add at the PM4
  level, where the meaning of a draw is lost.

A native renderer intercepts the game *above* the GPU — at the D3D API calls
(or the engine's own renderer) — and draws with Vulkan directly.

## What the binary looks like

From the XEX header and the recompiled code (`tools/` has the scripts):

- **XDK 2.0.7978.3**, statically linked: `D3D9`, `XGRAPHC`, `D3DX9`, plus Bink
  video (`BINK*` sections). No XUI.
- **XDK D3D library**: roughly `0x823A6400–0x823DE000`, ~1,100 functions. All
  `Vd*` kernel calls come from here (`VdInitializeRingBuffer` in
  `sub_823B2DC8`, `VdSwap` in `sub_823BA340`). Functions that build draw
  packets (`DRAW_INDX`/`DRAW_INDX_2`) include `sub_823AFD28`, `sub_823AFE80`,
  `sub_823B33E8`, `sub_823BC6D0`, `sub_823C0328`, `sub_823C4960`, but the game
  never calls them: its draws go through `DrawVertices` / `DrawIndexedVertices`
  (`sub_824DA698` / `sub_824DAA88`) in a second D3D span,
  `0x824D92E8–0x824DC3E8`.
- **Engine renderer** (Blue Castle Games, assert paths
  `c:\bcg\baseball\library\renderlib\...`):
  - `renderlib/common` from ~`0x82460000` (shader-constant managers, `cGFX`
    primitives, async loading),
  - `renderlib/xbox360` ~`0x8248F000–0x8249B000`: `hwmaterial.cpp`
    (`sub_824907A8`), `hwrender.cpp` (`sub_82493660`), `submitrender.cpp`
    (`sub_82494A80`), `drawrender.cpp` (`sub_82498BC0`) — a platform layer
    sitting directly on D3D;
  - `systemlib/xbox360/memoryvertexbuffer.cpp` ~`0x8240FE58`.
- **Shaders** are precompiled Xenos microcode shipped in
  `data/shaders/baseball-{sd,vd,pd,vs,ps,sc}.big`, looked up by hash
  ("compiled pixel shader def records"). There is no HLSL source, so a native
  renderer still has to translate Xenos microcode (or replace shaders by hand).

## Where to cut in

1. **D3D API level** (the Unleashed Recompiled model). Replace the XDK D3D
   entry points (`CreateTexture`, `SetTexture`, `SetVertexShader`,
   `DrawIndexedVertices`, `Resolve`, `Swap`, …) with Vulkan implementations.
   The XDK D3D API is documented, which gives a spec to implement against.
   Risk: parts of XDK D3D are inline in the headers and poke the device
   structure directly from engine code, so not every state change is a call.
2. **Engine level**: replace `renderlib/xbox360` (`cHWRender`, `cHWMaterial`,
   `cSubmitRender`, `cDrawRender`) with a Vulkan back end. Fewer, higher-level
   entry points with game meaning (materials, passes), but the engine is
   undocumented and has to be reverse-engineered first.

**Decision (phase 0 data): cut at D3D, at the draw.** From
[d3d-map.md](d3d-map.md):

- All ~1,340 draws per frame go through the two D3D draw functions, called only
  from renderlib's command executor; no engine code writes GPU packets.
- XDK D3D keeps the whole GPU state as a shadow in the device structure (Xenos
  register images, shader constants, fetch constants, object pointers) with
  dirty masks, and flushes it at each draw. The engine also writes that shadow
  inline in 5 places, which a plain "wrap every Set call" design would miss -
  but reading the shadow at the draw sees them.
- The engine level is wider and undocumented: 99 renderlib entry points used
  by game code plus a 42-opcode command format.

So the native renderer hooks the draw functions and reads the device shadow
(the dirty masks say what changed), and hooks the calls that need meaning the
registers don't carry: render target / depth surface setup, `Resolve`, `Swap`,
clears, and resource creation (textures, buffers, shaders). Setters for
constants, textures and render states don't need hooks of their own.


Either way, reuse from the SDK what isn't EDRAM-specific: Xenos texture
formats/untiling, the microcode → SPIR-V shader translator (offline, keyed by
the shader hashes in the `.big` files), Vulkan device/swapchain setup and the
SDK's "detached" presentation mode (no GPU plugin; the app supplies the
renderer via `OnCreateImmediateDrawer`).

## Phases

0. **Map** *(done)*. Trace build (below): per-frame call counts, call sites
   and call order for every D3D-library and renderlib function. Output:
   [d3d-map.md](d3d-map.md), [d3d-names.txt](d3d-names.txt) and the cut-point
   decision above.
1. **Shadow capture** *(validated 2026-10-17)*. `src/native_capture.cpp` hooks
   the two draw functions, `Clear`, `Resolve` and `Swap` without changing
   behaviour and records each frame as a native renderer would read it, checked
   against the PM4 stream D3D sends to the GPU: the device shadow holds the
   complete GPU state at every draw ([d3d-map.md](d3d-map.md#phase-1-shadow-capture-srcnative_capturecpp)).
   Also mapped: MSAA / predicated tiling (`BeginTiling`/`EndTiling` = one
   full-size 4x MSAA pass for a native renderer) and resources (created lazily
   from fetch constants; nearly everything static after loading, a few MB of
   dynamic vertex data per frame) - see [d3d-map.md](d3d-map.md#resources).
2. **Resources**. Textures (format conversion, untiling, mips), vertex/index
   buffers, vertex declarations, shaders translated to SPIR-V, pipeline cache.
3. **First native pixels**. Draw one simple pass natively — the 2D front end
   (menus, fonts) is the easiest — in a debug view beside the emulator.
4. **Full frame**. Render targets and resolves without EDRAM, depth, MSAA,
   post-processing, shadows, crowd, Bink movie textures.
5. **Switch over**. Run without the Xenos plugin (detached mode) with our own
   swapchain and overlays; keep the emulator as a fallback setting.
6. **Port features**. Widescreen/ultrawide, AA options, resolution scaling
   without EDRAM limits, frame pacing.

This is a long project — comparable in size to the renderers written for other
recompilation ports — and each phase is useful on its own.

## Phase 0: render tracing

Build a separate trace binary (doesn't touch the normal build):

```bash
cmake --preset linux-amd64-release -B out/build/linux-amd64-trace \
      -DCMAKE_CXX_COMPILER=clang++-20 -DTB2_RENDER_TRACE=ON
cmake --build out/build/linux-amd64-trace
```

It wraps all ~2,100 functions in the D3D library and renderlib ranges with a
call counter (generated by `tools/gen_render_trace.py`). Functions the game
already hooks in `src/` aren't wrapped (that would be a second definition):
their hook calls `TB2_TRACE_HOOKED(address)` instead, so the trace build runs
the same code as the release build. In game, press **F8**
(or `kill -USR2 $(pgrep -x thebigs2)`) to record the next frame. Each trace goes
to `~/.local/share/thebigs2/traces/render_trace_NNN.txt`:

- per function: calls, and call sites (return addresses) with counts;
- the full call sequence (frame, thread, function, return address).

Settings: `trace_frames` (frames per trace, default 1), `trace_frame_marker`
(function that ends a frame, default the swap `823BA340`), `trace_max_seconds`
(safety stop, default 5), `trace_at_start` (record the boot sequence).

Useful captures: main menu, batting view before the pitch, a pitch in flight,
a home-run replay, the stadium intro. To capture several scenes in one run,
send `kill -USR2` every few seconds while `tools/atbat.sh` plays (with
`BIN=out/build/linux-amd64-trace/thebigs2`) and take a screenshot each time.

Summarise traces with `tools/trace_summary.py`: the functions called from
outside a range (the API surface), with calls per trace and their callers,
named from `docs/d3d-names.txt`:

```bash
tools/trace_summary.py menu=.../render_trace_010.txt bat=.../render_trace_017.txt
tools/trace_summary.py --range renderlib bat=.../render_trace_017.txt
```
