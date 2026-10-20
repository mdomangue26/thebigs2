# Xenos shader translator (copied from the ReXGlue SDK)

The native renderer (`docs/native-renderer.md`) translates the game's Xenos
shader microcode to SPIR-V with the SDK's translator, which lives in the Xenos GPU
plugin. The plugin is loaded at run time and doesn't export it, so the sources
are copied here and built into the static library `tb2_xenos`.

- Source: `rexglue-sdk` branch `fix/stability` at `e806d48` (based on
  `development` bd833a2), `src/graphics/`:
  `pipeline/shader/{shader,translator,translator_disasm,spirv,spirv_builder,
  spirv_translator*}.cpp` and `format/ucode.cpp`; for the renderer (phase 3)
  `register_file.cpp`, `registers.cpp` and `draw_util.cpp` (from
  `util/draw.cpp`: viewport, scissor, depth / color state helpers, without the
  resolve and memexport parts). Licence: `LICENSE` (BSD,
  ReXGlue and Xenia).
- `glslang/SPIRV/`: the glslang headers `SpvBuilder.h` needs (the SDK installs
  `libSPIRV.a` but not its headers), from the SDK's glslang (`f4f1d8a3`).
  Licence: `glslang/LICENSE.txt`.

Changes from the SDK:

- No GPU plugin cvars: `spirv_cube_lod_from_gradients` is always on (the 2x
  NVIDIA hang fix, SDK `a52de02`), `dump_shaders` is gone, and `draw_util`'s
  `half_pixel_offset` is always on.
- `draw_util_constants.cpp`: the two sample-position tables the translator
  uses from the SDK's `util/draw.cpp`.

The translated shaders need Vulkan's `scalarBlockLayout` feature (1.2): the
SDK's `SystemConstants` uniform block isn't std140-aligned (e.g. the
`user_clip_planes` vec4 array at offset 28). `spirv-val` rejects it otherwise;
the SDK's Vulkan device enables the feature (so does the native renderer's,
which is created through the SDK's `VulkanProvider`).
