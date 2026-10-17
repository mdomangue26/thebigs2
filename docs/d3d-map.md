# How the game renders: D3D and renderlib map (phase 0)

Results of the render-trace mapping in [native-renderer.md](native-renderer.md).
Names are in [`d3d-names.txt`](d3d-names.txt); `?` marks a name that fits the
evidence below but hasn't been confirmed against the XDK semantics. Counts are
calls per frame from the trace build at 1x, default settings (`scene_msaa` on),
2026-10-17 (`out/perf/rtrace3`, Yankees @ Red Sox, Fenway, night).

## The layers

```
game code (scene graph, players, UI)
   │  99 renderlib entry points (~13k calls/frame at the plate)
   ▼
renderlib (BCG engine, 0x82460000–0x8249B800)
   │  records render command lists; sub_8249AF80 replays them
   │  (42 opcodes: 7 = DrawVertices, 8 = DrawIndexedVertices, ...)
   │  ~80 XDK D3D functions + inline XDK code (5 functions)
   ▼
XDK D3D (0x823A6400–0x823DE000 and 0x824D92E8–0x824DC3E8)
   │  device shadow state + dirty masks, flushed into PM4 at each draw
   ▼
PM4 ring buffer -> SDK Xenos GPU emulation (command processor, EDRAM, ...)
```

## XDK D3D: how state reaches the GPU

The XDK D3D device keeps a **shadow of the GPU state** in the device structure
(pointer at `renderer + 408`; the renderer singleton is at `0x829CD6AC`). Set
calls only write the shadow and set a bit in a dirty mask. The two draw
functions flush every dirty group into the ring as PM4 packets and then write
the draw packet. Device layout found so far:

| Offset | Contents |
|---|---|
| `+0` (u64) | dirty mask: vertex shader float constants (4 registers per bit) |
| `+8` (u64) | dirty mask: pixel shader float constants |
| `+16` (u64) | dirty mask: render-state register groups |
| `+24` (u64) | dirty mask: fetch constants (textures, vertex streams) |
| `+32` (u64) | dirty mask: more register groups |
| `+48` / `+52` / `+56` | PM4 ring: **last dword written** (D3D pre-increments: next free = value + 4) / segment limits; `sub_823B2A50` moves to the next segment |
| `+1152 + 24*n` | fetch constants, 32 slots x 6 dwords (registers `0x4800..`; textures and vertex buffers) |
| `+1920` | ALU constants, registers `0x4000..`: vertex shader c0–c255, then pixel shader c0–c255 at `+6016` |
| `+10112` | bool + loop constants (registers `0x4900..0x4927`) |
| `+10272` | user clip planes `PA_CL_UCP_0..5` (registers `0x2388..0x239F`) |
| `+10368`.. | register images, one dirty bit per register: `0x2000..0x200F` at `+10368`, `0x2100..0x2114` at `+10444`, `0x2180..0x2184` at `+10528`, `0x2200..0x220B` at `+10548`, `0x2280..0x2294` at `+10596`, `0x2300..0x2325` at `+10680`, `0x2380..0x2387` at `+10832` |
| `+11824` | vertex declaration? (`sub_823BCEE8`) |
| `+12448` | render targets 0–3 (`D3DSurface*`, `SetRenderTarget`; also copies the surface's `+28` word into `RB_COLOR_INFO`/`RB_COLORn_INFO`) |
| `+12464` | depth/stencil surface |
| `+12552 + 4*s` | texture object per fetch slot (`SetTexture`: `device[3138 + s]`) |
| `+12700` / `+12704` | pixel shader / vertex shader objects |
| `+13524` / `+13528` | a separate resource-reference list (not PM4) |

All of this is verified at run time by the shadow capture (below).

So the shadow at draw time **is** the Xenos register state (plus the D3D objects
behind it). Render states map to the packed register blocks, e.g. `+10548` =
`RB_DEPTHCONTROL` (0x2200), `+10552` = `RB_BLENDCONTROL0`, `+10556` =
`RB_COLORCONTROL`, `+10568` = `PA_SU_SC_MODE_CNTL`, `+10500` = `RB_ALPHA_REF`,
`+10832..` = polygon offset.

### Draws

All draws go through two functions, both called only from renderlib:

| Function | menu | stadium intro | at the plate | fielding |
|---|---:|---:|---:|---:|
| `824DAA88` DrawIndexedVertices | 0 | 161 | 1,037 | 758 |
| `824DA698` DrawVertices | 367 | 151 | 303 | 129 |
| `823C6450` Resolve | 1 | 16 | 16 | 16 |
| `823C5C28` Clear | 7 | | 8 | |
| `823B68D0` SetRenderTarget | 120 | 132 | 132 | 132 |

The XDK's other packet builders (`823AFD28`, `823BC6D0`, `823C0328`, ...) are
never called in these scenes. With predicated tiling (3 tiles for the main scene
at 4x MSAA) the GPU sees each scene draw three times: ~1,340 D3D draws become
the ~3,640 draws the command processor counted.

### API surface used per frame (at the plate)

78 D3D functions are called from outside the library; the busy ones:

| Function | Calls | Called from |
|---|---:|---|
| `823BC8E8` SetPixelShaderConstantF | 8,727 | `82492B08` (per material constant upload) |
| `823BC810` SetVertexShaderConstantF | 3,552 | `82492A10`, executor |
| `823AF7B0` SetTexture | 2,567 | `82493660` |
| `823B6068` SetStreamSource? | 1,174 | `82491188` |
| `823B6188` SetIndices? | 807 | `82491268` |
| 8 render-state setters (`823B44F0`..`823B46D0`) | 700 each | `82491598` (per material) |
| `823BCD18` / `823BCB58` SetVertexShader / SetPixelShader | ~250 each | `824937D0` / `82493850` |
| sampler state setters (`823B5548`, `823B56F0`, ...) | ~400 | `82490E08` |

Evidence for the names: `SetTexture` stores the texture at `device[3138+s]`,
merges its header into fetch constant `48+s` and sets `+24`; the constant
setters copy float4s to `device + 1920/6016 + 16*reg` and OR the caller's mask
into `+0` / `+8`; the render-state setters write single register images and set
`+16` bits.

### Inline XDK code in the engine

Static scan of every non-D3D function for stores through the device pointer
(`renderer + 408`) and for PM4 headers:

- No engine function writes PM4 packets; all packets come from the D3D library.
- 5 renderlib functions write the device shadow directly (XDK inline
  functions): `82490E08` / `82490FF0` / `82498220` (fetch-constant words + `+24`
  mask: sampler states), `82492C00` (vertex constants c168–c169 + `+0` mask),
  `82493950` (c170 + mask, then DrawVertices: a full-screen quad helper).

These bypass any D3D call wrapper, but land in the shadow that the draw flushes.

## renderlib

- `8249AF80` executes the engine's recorded command lists (jump table, 42
  opcodes, recursive for nested lists). Opcode 7 = DrawVertices, 8 =
  DrawIndexedVertices; other cases call the material/constant/texture helpers
  above.
- 99 renderlib functions are called from game code (`tools/trace_summary.py
  --range renderlib`), most from the scene graph and UI; the top one
  (`82461C30`, 8.5k/frame) is a constant/matrix manager.
- Pass setup: `82495A90` (render targets, viewport, clear), `82496020`
  (render target + `Resolve`), `82219148` sets up the scene passes (MSAA and
  tiling, see `src/render_tweaks.cpp`).

## Phase 1: shadow capture (`src/native_capture.cpp`)

F6 (or `kill -s RTMIN+1 <pid>`) records `native_capture_frames` frames to
`<user data>/captures/native_capture_NNN.txt`: one line per draw (arguments,
primitive and index count from the draw packet, shaders, render targets and
surface registers, depth/blend/cull state, bound textures), clear and resolve,
the frame's render passes, and a validation section. `tools/native_capture_run.sh`
captures every few seconds while `tools/atbat.sh` plays.

Validation: the capture parses the PM4 packets D3D writes to the ring (draws,
clears, resolves and everything between, segment by segment) and checks them
against the shadow decoding above. Over 58 frames (intro video, menus, stadium
intro, batting, fielding; 28,607 draws), 2026-10-17:

- every hooked draw wrote exactly one draw packet; the only other draw packets
  are inside `Clear` (8/frame) and `Resolve` (16/frame), both rectangle lists;
- every register/constant the draw flush wrote equals the shadow value
  (~270k checks per batting frame), except shader literal constants, which
  come from memory via `LOAD_ALU_CONSTANT` (pixel c254–c255);
- the GPU state rebuilt from all packets equals the shadow at every draw
  (67 million register comparisons, 0 mismatches).

So **a native renderer can read the complete GPU state from the device
shadow at each draw**. Registers written outside the shadow: `VGT_INDX_OFFSET`
(`0x2102`, from the draw's arguments), `0x5000–0x5002` (per draw, constant
`0, 0x25000, 0`; unnamed in the SDK's register table), coherency registers
`0x0A2F–0x0A31` (cache flushes). One earlier run had a single parse desync
(1 draw in ~44k, not reproduced since; the capture now logs details if it
recurs).

### A batting frame (1x, default settings)

From the render passes and resolves in a capture (`si` = `RB_SURFACE_INFO`,
pitch in the low 14 bits):

1. 24 draws, 320 wide, cleared to magenta -> resolve (environment/reflection?)
2. 44 draws, 560 wide, no depth -> resolve
3. 560 wide with depth -> depth resolve + color resolve
4. 30 depth-only draws, 1600 wide (shadow map) -> depth resolve
5. main scene, 1280 wide: ~1,080 draws alternating two EDRAM color formats
   (`RB_COLOR_INFO` format 3 / 12) -> depth + color resolves
6. post-processing chain 1280 -> 320 -> 80 -> 80 -> 320 -> 160 (1–2 draws each,
   resolve after each)
7. HUD/UI: 90 draws into the front-buffer target, final resolve -> Swap

The main scene's surface registers say 1x: the game's 4x MSAA and 3-tile
predicated tiling are applied by `BeginTiling` / `EndTiling` (three
`INDIRECT_BUFFER` replays per frame), which a native renderer needs to handle
explicitly.

## Open questions

- Confirm the remaining `?` names (render states, samplers, streams).
- How `BeginTiling`/`EndTiling` set up MSAA and the tiles.
- Resource creation (textures, vertex/index buffers, shaders) happens at load
  time; map it from a loading-screen trace (`trace_at_start`, or F8 during a
  load with a larger `trace_frames`).
- `824D95C0` (182/frame from `memoryvertexbuffer.cpp` `8240FA48..`) and
  `824D9570`: small functions in the second D3D span, purpose unknown.
- Bink video draws through `824DA078/80/90` (called from `824BA8F8..824BB368`).
