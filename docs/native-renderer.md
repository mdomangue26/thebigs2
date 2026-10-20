# Native Vulkan renderer — plan and findings

Status: **Phases 0-5 done: the game can run on the native renderer
(`native_renderer = true`, experimental), with the emulator's GPU drawing
nothing (`native_detached = true`); frames within 1/255 of the emulator's.
Restarted 2026-10-20 on [Plume](#rendering-api-plume) (phases 3-5 redone the
same day); phase 6 (port features) next.** Work happens on the
`native-renderer` branch; the emulator stays the default. A first renderer
written straight against Vulkan (phases 3-6, up to resolution scaling, its
own render thread and AA options) is kept on the branch `native-renderer-vk`
for reference. Findings: [d3d-map.md](d3d-map.md).

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
2. **Resources** *(conversion done 2026-10-17, see below)*. Textures (format
   conversion, untiling, mips), vertex/index buffers, shaders translated to
   SPIR-V (`src/native/`), checked against every draw of captured frames. The
   pipeline cache moves to phase 3 (it needs a Vulkan device).
3. **First native pixels** *(done, on Plume 2026-10-20, see below)*. The 2D
   front end (menus, fonts, Bink video) drawn natively from captured frames,
   compared with the emulator.
4. **Full frame** *(done, on Plume 2026-10-20, see below)*. Render targets and
   resolves without EDRAM, depth, MSAA, post-processing, shadows, crowd, Bink
   movie textures, checked resolve by resolve against the emulator.
5. **Switch over** *(done, on Plume 2026-10-20, see below)*. Live rendering
   every frame, presented through the SDK presenter (its pacing, overlays and
   post effects stay) with the guest's gamma ramp; the emulator's GPU can stop
   drawing (detached). The Xenos plugin still runs the command stream (fences,
   interrupts, gamma ramp registers); it remains the default renderer.
6. **Port features** *(in progress on Plume, see below)*. Resolution scaling
   without EDRAM limits *(done)*; next its own render thread and AA options
   (both done in the first renderer), then widescreen/ultrawide, frame pacing.

This is a long project — comparable in size to the renderers written for other
recompilation ports — and each phase is useful on its own.

## Rendering API: Plume

The renderer is written against [Plume](https://github.com/renderbag/plume)
(RT64's rendering API: Vulkan, D3D12, Metal), copied into `src/native/plume/`
with local changes (README there). Only its Vulkan backend is built.

- **Device**: Plume wraps the SDK's VkDevice instead of creating its own
  (`src/native/plume_device.cpp`, local `VulkanExternalDeviceDesc` patch), so
  the renderer's images can go to the SDK's presenter (ImGui overlays, gamma
  ramp, frame pacing) and both submit to one queue under the SDK's queue
  mutex. Plume allocates through VMA.
- **Shaders**: SPIR-V from the Xenos translator, as before. Plume's D3D12
  backend only takes DXIL, so a D3D12 path would need SPIR-V -> HLSL -> DXIL at
  run time; Windows stays on Vulkan.
- **Check**: `--native_plume_selftest=true` wraps the SDK device at startup
  and checks a render-target clear read back through a buffer, and a buffer
  copy. 2026-10-20 (RTX 4060): passes; with `--vulkan_validation_enabled=true`
  no messages beyond the SDK's own (the same four VUIDs without Plume).
- Things to watch: Plume bakes the stencil reference into the pipeline (more
  pipeline variants than with dynamic state); its barrier/layout tracking is
  still marked as being refined upstream.

## Phases 3 and 4 on Plume (`src/native/renderer.h`)

The renderer draws a frame from the same inputs the capture reads: at every
draw the GPU state is rebuilt from the D3D device shadow as a register file,
plus the draw's shader microcode and draw packet; Clear and Resolve are hooked
too. It runs on the SDK's Vulkan device through Plume (the emulator keeps
rendering beside it) and records the frame into two command lists (texture
uploads, then the draws) submitted at the frame's Swap. Ported 2026-10-20 from
the first renderer (`native-renderer-vk`, commit 0eff77f), which wrote the same
logic against raw Vulkan.

- **Draw path**: a lean copy of the SDK's Vulkan backend for its host render
  target path (`VulkanCommandProcessor::IssueDraw` / `UpdateSystemConstantValues`
  / `UpdateBindings`, `VulkanPipelineCache`), so the translated shaders get the
  same system constants and bindings as in the emulator. Viewport / scissor /
  depth / color-mask helpers are the SDK's `draw_util`, copied into
  `src/native/xenos/`. Pipelines are cached by their full state; stencil
  reference / masks, blend constants and depth bias are dynamic (local Plume
  additions). Plume framebuffers (render pass objects) are cached per set of
  targets.
- **Render targets**: one texture per EDRAM surface (base, format, pitch), 2048
  rows tall; no EDRAM emulation. Formats that view the same EDRAM data
  (`2_10_10_10_FLOAT` / `..._AS_16_16_16_16`, 8888 / gamma, `2_10_10_10` /
  `AS_10_10_10_10`) share one. 8-bit gamma targets are gamma-encoded in the
  shader (as the SDK does); 16-bit fixed formats use float textures. Depth is
  D32_FLOAT_S8_UINT (AMD has no D24S8). MSAA targets get the guest sample
  count.
- **Resolves** draw a fullscreen triangle (`glsl/resolve*.frag`) that reads
  the render target region into the destination texture as a color target:
  the attachment converts the format, MSAA color is averaged, depth takes
  sample 0 into an R32_FLOAT texture (as the SDK's texture cache loads 24-bit
  depth). Destinations are keyed by address and updated in place; later draws
  sample them, composing the sampling fetch swizzle with the inverse of the
  destination's. Predicated tiling is drawn once at full size; each per-tile
  resolve copies to its destination point and clears only its rectangle.
  Every resolve is read back for comparison.
- **Vertex data**: each vertex fetch constant's buffer is copied (deduplicated
  per frame) into a storage buffer standing in for the SDK's 512 MB shared
  memory, and the fetch constants handed to the shader point into it.
- **Constants**: one constants descriptor set per draw (reused frame after
  frame), its buffers at the draw's offsets in a per-frame arena. Float
  constants are packed per shader as the SDK does. Literal constants come from
  the shader objects ([d3d-map.md](d3d-map.md#shaders)).
- **Textures** convert through phase 2's `ConvertTexture`, cached by fetch
  constant shape and content hash. Views carry the fetch constant's swizzle;
  2D textures are sampled as arrays; signed bindings get SNORM views of the
  same bits (mutable-format textures).
- **Registers outside the shadow** come from the emulator's register file at
  the start of the frame and the packets of the frame (D3D sets the window
  scissor per render target).
- The SDK's `anisotropic_override` applies to the native samplers as the SDK
  applies it.

```bash
tools/native_capture_run.sh out/perf/ncapN 15 16 --native_capture_render=true \
    --readback_resolve=full --resolution_scale=1   # scene_msaa: on at 1x
#   [--vulkan_validation_enabled=true: validation layer on the SDK device]
tools/native_compare.py out/perf/ncapN/data/captures/native_capture_009_native
```

With the SDK's `--readback_resolve=full` the emulator writes each resolve back
to guest memory; the capture reads the destination textures shortly after the
frame's Swap (`emu_resolve_KK_*.png`, in render target order) next to the
native `resolve_KK_*.png` (the whole destination texture after that resolve);
`native_compare.py <dir>` compares each pair. Only the last resolve into each
texture survives in memory. Half-float resolves are also dumped raw
(`*.rgba16f`) since PNGs clip above 1.

Results (2026-10-20, RTX 4060, 1x with 4x MSAA, `out/perf/pcap1`-`pcap6`, Bink
video, profile dialog, menus, cutscene, gameplay):

- Every draw drawn (up to 1,730 per frame, 29-36 resolves), 0 texture
  failures; with the validation layer no messages beyond the SDK's own four.
- Menus and video: final frame within 0.03-0.21/255 of the emulator (raw, no
  tone curve).
- Gameplay: the passes (render-to-texture boards, shadow map, scene color,
  bloom, luminance chain) within 0.0-0.3/255 after the tone curve; the scene's
  8-bit encode 1.1-1.5/255 (1% of pixels > 24: the 7e3 precision below).
  `native_compare`'s raw "85/255" on the 16_16 luminance chain is the PNG's
  blue channel (0 here), not the image.
- **Depth resolves read as color**: the composite pass samples the scene
  depth resolve's memory (`0A1EF000`) as `8_8_8_8` and blends its two
  color-grading LUTs by the stencil byte; the depth-of-field passes of
  cutscenes read it too. Depth resolves therefore also keep the console's
  memory words, `(depth24 << 8) | stencil` (24-bit unorm or 20e4, per the EDRAM
  depth format), drawn by `glsl/pack_depth*.frag` from the render target's
  depth and stencil (a stencil-aspect view) into an `R32_UINT` texture that is
  viewed in the fetch's format; depth fetch formats still get the float
  texture. (The first renderer found this in its phase 5 and packed with a
  compute pass over buffer copies.) Before it the final gameplay frames were
  4-7/255 off and cutscenes lacked their depth of field; with it (`out/perf/
  pcap4`, `pcap6`, 4 cutscene and 10 gameplay frames) final frames are
  0.14-0.45/255 raw.
- Captures right at a cutscene's start can compare different moments: the
  emulator's resolves are read from memory after the Swap, while the crowd is
  still streaming in (`pcap4` frame 8: crowd missing natively, matching in
  every later cutscene frame).

Known differences:

- 7e3 precision and the 2-bit alpha of `2_10_10_10_FLOAT` (half floats here,
  like the SDK's host render target path); shows mostly in the 8-bit encode of
  the HDR scene.
- EDRAM aliasing: a resolve of a surface nobody drew this frame (a cutscene
  frame without a shadow pass) returns other passes' EDRAM bits in the
  emulator, a cleared image here.
- Temporal effects (auto-exposure adapts from the previous frame's 1x1
  luminance): in captures the previous frame's resolves come from guest
  memory; continuous rendering (phase 5) has to keep resolved textures across
  frames.
- Not handled: memexport, line polygon mode (drawn filled, noted), gaps
  between bound color targets (skipped, noted). None occur in the captures.

## Phase 5 on Plume: switch over

```bash
--native_renderer=true            # live: native frames on screen, emulator GPU still draws
--native_detached=true            # with it: the emulator's GPU draws nothing (gpu_skip_draws)
--native_stats_interval=300       # log draws / resolves / GPU time every 300 frames
```

Ported 2026-10-20 from the first renderer's phase 5 (commit 3823000 on
`native-renderer-vk`; its capture-side code came over unchanged).

- **Live rendering**: the capture hooks (`src/native_capture.cpp`) feed the
  renderer every frame, without the capture's checks (ring parsing for shader
  loads, registers outside the shadow and draw packets stays). Two frames in
  flight, each with its own command lists, fence, arenas and descriptor sets;
  captures (F6) still work in live mode and read back that frame.
- **Presentation**: at Swap the front buffer (the texture object Swap gets,
  r4) is drawn through the guest's 256-entry gamma ramp (DC_LUT_30_COLOR, from
  the command processor; `glsl/present_gamma_table.frag`) into the SDK
  presenter's guest output image (`Presenter::RefreshGuestOutput`, the same
  hand-over the emulator uses), so pacing, FSR/CAS, overlays and screenshots
  keep working. The image the presenter lends is wrapped as a Plume texture
  (`WrapImage` in `plume_device.cpp`: Plume's non-owning `VulkanTexture`, as
  for swapchain images), and the frame is submitted inside the hand-over.
- **SDK switches** (fix/stability f5658a7): `present_guest_swaps=false` stops
  the emulator's present (its swap still ends its frame), `gpu_skip_draws=true`
  makes the command processor skip draws and resolves; the gamma ramp
  accessors are public. The native renderer sets both itself.
- **Persistent resolved textures**: kept across frames (auto-exposure, the
  depth-of-field and bloom ping-pong read last frame's). A resolved texture is
  dropped when the CPU puts something else in its memory (16 x 64 bytes of it,
  sampled at the resolve, no longer match). Captures without live rendering
  start each frame without them. Live comparisons with the emulator's
  resolves in memory: `--readback_resolve=full --native_keep_resolves=true`.
- **CPU cost**: textures and vertex buffers are re-hashed / re-copied only when
  guest pages they cover were written since (SDK physical memory invalidation
  callbacks with per-page write epochs, `native_page_watch`).
- GPU time per frame comes from timestamp queries (Plume query pools).

Results (2026-10-20, RTX 4060, Fenway at-bat at 1x with 4x MSAA): screenshots
of the native frames against the emulator's (`out/perf/plive1` vs `pemu1`,
`tools/atbat.sh`) 0.69-0.93/255 (the first renderer: 2.0-2.3/255); every draw
drawn, 0 texture failures; GPU 8-10 ms per frame at the plate; the validation
layer reports nothing beyond the SDK's own messages. 60 fps benchmark
(`tools/perf/bench.sh`, plate):

| | GPU command thread | game render thread | fps |
|---|---|---|---|
| emulator (2026-10-15) | 12.0 ms | 6.4 ms | 60 |
| native, detached, hashing every frame | 2.6 ms | 23.8 ms | 42 |
| native, detached, page watch | 1.7 ms | 15.4 ms | 60 |

The native work runs on the render thread (draw recording, index conversion,
vertex copies, driver), so it adds to the game's frame time instead of running
beside it like the command processor did. Next (phase 6 of the first
renderer): record on a thread of its own, resolution scaling, AA options.

## Phase 6 on Plume: port features

Ported 2026-10-20 from the first renderer's phase 6 (commits 747034d and
0f36190 on `native-renderer-vk`; capture-side code and the shader library
change came over unchanged).

### Resolution scaling

The native renderer follows `resolution_scale` (1-4): render targets are
`pitch x scale` wide, resolves draw scaled rectangles into scaled textures, and
the front buffer goes to the presenter at its scaled size. Draws get what the
SDK's Vulkan backend applies: `GetHostViewportInfo` with the scale, scaled
scissor, depth bias slope and point sizes, and the shader library's translator
is created with the scale (pixel positions and the coordinates / sizes of
resolution-scaled textures are converted back to guest pixels in the shaders,
for textures flagged in `textures_resolution_scaled` - every resolved texture).
Texture descriptors are set up before the system constants, which carry those
flags.

Render targets don't get 2048 rows (EDRAM surfaces have no height): each draw
asks for the rows its scissor and viewport reach, each resolve for its
rectangle, and a target grows (new texture, old contents copied, framebuffers
using it retired) when asked for more. At 2x the 4x MSAA scene would otherwise
need ~670 MB. Depth resolves are packed with stencil only once something has
sampled them as color. Occlusion query draws (`PA_SC_VIZ_QUERY`) are dropped,
as the SDK does. GPU time per resolve is in the stats (`native_stats_interval`).

Results (2026-10-20, RTX 4060, Fenway at-bat at 2x, `scene_msaa` off as the 2x
default; screenshots against the emulator's own 2x FSI frames, `out/perf/plive5`
vs `pemu2`): 0.79-1.58/255 (the first renderer: 1.9-2.8). 1x unchanged
(0.69-1.03/255). 60 fps benchmark at 2x: emulator 48 fps (GPU-bound, FSI path,
command processor 12.5 ms of work per frame), native detached 60 fps with 15.1
ms of render thread time.

### Fixed along the way (first renderer): wrong vertex shaders

Frames without predicated tiling (`scene_msaa` off: 1x without MSAA, every 2x
frame) sometimes drew a few meshes with another draw's vertex shader - a giant
baseball across the screen in about half the frames. The current shaders were
tracked from the microcode loads parsed out of the ring, and some loads happen
where the parser doesn't look; which frames lost one depended on timing. Now a
draw whose flush has no load uses the shader D3D has bound, through the
(shader object, vertex declaration) -> microcode map learned from earlier
loads (`src/native_capture.cpp`).

Found with `native_debug_frames` (per-draw logs of frames whose front buffer
has an unusually bright patch, next to the last normal frame's, written to the
captures folder). Other debug settings: `native_sync_frames` (no frames in
flight), `native_debug_skip_draws=FIRST-END` (bisecting - unreliable for
intermittent problems).

## Phase 2: resource conversion (`src/native/`)

Code the native renderer will use, tested on captured frames (F6, see phase
1) while the emulator still renders. Nothing here needs a GPU yet.

- **Shaders** (`shaders.h`): microcode -> SPIR-V with the SDK's translator,
  copied into `src/native/xenos/` (the GPU plugin doesn't export it; see the
  README there). Vertex shader microcode comes from the load packet D3D's
  flush writes at the draw (D3D patches vertex shaders per vertex
  declaration); pixel shader microcode is also reachable from the
  `D3DPixelShader` object ([d3d-map.md](d3d-map.md#shaders)). Modifications
  per vertex + pixel shader pair, as the SDK's Vulkan backend picks them.
  The translated shaders need `scalarBlockLayout` (Vulkan 1.2; NVIDIA and
  RADV have it): the SDK's system-constant uniform block isn't std140-aligned,
  which `spirv-val` rejects otherwise (drivers accept it in the emulator).
- **Textures** (`textures.h`): fetch constant -> untiled, byte-swapped data in
  a Vulkan format with all mips/layers laid out for `vkCmdCopyBufferToImage`
  (DXT1/2_3/4_5/DXN/DXT5A as BC1/2/3/5/4, CTX1 converted to R8G8, 8/16/32-bit
  and float formats as is). Sizes, mip offsets and packed mip tails come from
  the SDK's `texture_util`. Data stays unsigned; signedness, gamma and swizzle
  are applied when sampling (as in the SDK).
- **Geometry** (`buffers.h`): index buffers byte-swapped, quad lists turned
  into triangle lists (auto-indexed quads get generated indices). Vertex
  buffers upload raw: the translated shaders fetch and byte-swap vertices
  themselves, from a storage buffer addressed through the fetch constants (the
  native renderer rewrites the addresses in the fetch constants it passes).

The capture checks all three on every draw of the frame and writes the
results (and, with `--native_capture_textures=<edge>`, PNG previews of every
converted texture) next to the capture:

```bash
tools/native_capture_run.sh out/perf/ncapN 25 7 --native_capture_textures=256
tools/native_validate.sh out/perf/ncapN/data/captures      # spirv-val every shader
tools/native_contact_sheet.py out/perf/ncapN/data/captures/native_capture_006_textures sheet.png
```

Results (2026-10-17, menus, intro video, batting and fielding frames):

| | batting frame (1,272 draws) | all frames (`out/perf/ncap21`-`27`) |
|---|---|---|
| textures | 388 converted, 0 failed | 0 failures; contact sheets look right (logos, faces, jersey numbers, normal maps, video planes) |
| shaders | 69 vertex + pixel pairs, 97 translations, 0 failed | 112 unique SPIR-V modules, all pass `spirv-val` (Vulkan 1.2 + scalar block layout) |
| shader source | 0 draws with unknown microcode; 217 pixel shader loads match the `D3DPixelShader` layout | same |
| geometry | 1,272 draws converted (33 with generated quad indices), 0 failed | 0 failures |
| vertex fetches | 1,163 checked against the fetch constant's buffer size: 0 out of range; 399 use a computed index (instancing, not checkable) | 0 out of range |

Not done yet: the pipeline cache (needs the device, phase 3); signed / gamma
textures and the swizzles (sampling side); the vertex shader per vertex
declaration taken from the shadow alone (only needed if the D3D flush ever
stops running).

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
