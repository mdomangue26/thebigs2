// thebigs2 - native renderer: Xenos shader microcode -> SPIR-V
// (docs/native-renderer.md, phase 2).
//
// Shaders are identified by their microcode (the game's vertex shaders are
// patched per vertex declaration by D3D, so the same D3D object can produce
// several). Translation uses the SDK's SPIR-V shader translator (copied into
// src/native/xenos, see the README there), with the bindings it defines: vertex
// data fetched by the shader from a storage buffer, constants and fetch
// constants in uniform buffers, textures and samplers per fetch constant.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>

namespace tb2::native {

using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;

// Register state that selects the translation of a vertex + pixel shader pair.
struct ShaderPairState {
  rex::graphics::reg::SQ_PROGRAM_CNTL sq_program_cntl;
  rex::graphics::reg::SQ_CONTEXT_MISC sq_context_misc;
  rex::graphics::reg::SQ_INTERPOLATOR_CNTL sq_interpolator_cntl;
  rex::graphics::xenos::MsaaSamples msaa_samples;  // RB_SURFACE_INFO
  rex::graphics::xenos::PrimitiveType primitive_type;
};

struct ShaderPairModifications {
  SpirvShaderTranslator::Modification vertex, pixel;
};

class ShaderLibrary {
 public:
  // Default: all optional translator features on (for offline checks); the
  // renderer passes its device's features.
  ShaderLibrary();
  explicit ShaderLibrary(const SpirvShaderTranslator::Features& features,
                         uint32_t resolution_scale = 1);
  ~ShaderLibrary();

  // The analyzed shader for big-endian microcode, created on first use.
  SpirvShader* Get(rex::graphics::xenos::ShaderType type, std::span<const uint32_t> ucode_be);

  // Translation modifications for a draw (as the SDK's Vulkan pipeline cache
  // chooses them, for host render targets without float24 depth emulation).
  // pixel may be null (depth-only draws).
  ShaderPairModifications Modifications(const SpirvShader& vertex, const SpirvShader* pixel,
                                        const ShaderPairState& state) const;

  // The translation of a shader with a modification, translated on first use;
  // null if translation failed.
  SpirvShader::Translation* Translate(SpirvShader& shader,
                                      SpirvShaderTranslator::Modification modification);

  size_t size() const { return shaders_.size(); }

 private:
  std::unique_ptr<SpirvShaderTranslator> translator_;
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>> shaders_;  // type + ucode hash
};

}  // namespace tb2::native
