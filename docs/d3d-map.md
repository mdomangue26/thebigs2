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
| `+48`.. | flush bookkeeping read by the flush helpers |
| `+1152 + 24*n` | fetch constants (6 dwords each; textures and vertex buffers) |
| `+1920` | vertex shader float constants c0–c255 (16 bytes each) |
| `+6016` | pixel shader float constants c0–c255 |
| `+10368` | register images, written as PM4 `SET_CONSTANT` blocks: `+10368` = Xenos regs `0x2000..`, `+10444` = `0x2100..`, `+10528` = `0x2180..`, `+10548` = `0x2200..`, `+10596` = `0x2280..`, `+10680` = `0x2300..`, `+10832` = `0x2380..` |
| `+12552 + 4*s` | texture object per sampler (`SetTexture`: `device[3138 + s]`) |
| `+13524` / `+13528` | ring buffer write pointer / reserve limit |

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
| `823B68D0` SetRenderTarget? | 120 | 132 | 132 | 132 |

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
| `823BCB58` / `823BCD18` SetVertexShader? / SetPixelShader? | ~250 each | `82493850` / `824937D0` |
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

## Open questions

- Confirm the `?` names (arguments against XDK D3D signatures; the render
  target/depth/viewport group in particular).
- Resource creation (textures, vertex/index buffers, shaders) happens at load
  time; map it from a loading-screen trace (`trace_at_start`, or F8 during a
  load with a larger `trace_frames`).
- `824D95C0` (182/frame from `memoryvertexbuffer.cpp` `8240FA48..`) and
  `824D9570`: small functions in the second D3D span, purpose unknown.
- Bink video draws through `824DA078/80/90` (called from `824BA8F8..824BB368`).
