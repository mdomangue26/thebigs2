// thebigs2 - native renderer: Xenos shader microcode -> SPIR-V (see shaders.h).

#include "shaders.h"

#include <xxhash.h>

#include <rex/string/buffer.h>

namespace tb2::native {

namespace xg = rex::graphics;

ShaderLibrary::ShaderLibrary() : ShaderLibrary(SpirvShaderTranslator::Features(true)) {}

ShaderLibrary::ShaderLibrary(const SpirvShaderTranslator::Features& features,
                             uint32_t resolution_scale) {
  // Host render targets: no EDRAM emulation in the shaders. With resolution
  // scaling, pixel positions and the coordinates / sizes of scaled (resolved)
  // textures are converted back to guest pixels inside them.
  translator_ = std::make_unique<SpirvShaderTranslator>(
      features, /*native_2x_msaa_with_attachments=*/true,
      /*native_2x_msaa_no_attachments=*/true, /*edram_fragment_shader_interlock=*/false,
      resolution_scale, resolution_scale);
}

ShaderLibrary::~ShaderLibrary() = default;

SpirvShader* ShaderLibrary::Get(xg::xenos::ShaderType type, std::span<const uint32_t> ucode_be) {
  const uint64_t hash = XXH3_64bits(ucode_be.data(), ucode_be.size_bytes());
  auto& slot = shaders_[hash ^ (uint64_t(type) << 63)];
  if (!slot) {
    slot = std::make_unique<SpirvShader>(type, hash, ucode_be.data(), ucode_be.size());
    rex::string::StringBuffer disasm;
    slot->AnalyzeUcode(disasm);
  }
  return slot.get();
}

ShaderPairModifications ShaderLibrary::Modifications(const SpirvShader& vertex,
                                                     const SpirvShader* pixel,
                                                     const ShaderPairState& state) const {
  ShaderPairModifications m;
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pixel ? vertex.writes_interpolators() &
                  pixel->GetInterpolatorInputMask(state.sq_program_cntl, state.sq_context_misc,
                                                  param_gen_pos)
            : 0;

  m.vertex = SpirvShaderTranslator::Modification(translator_->GetDefaultVertexShaderModification(
      vertex.GetDynamicAddressableRegisterCount(state.sq_program_cntl.vs_num_reg)));
  m.vertex.vertex.interpolator_mask = interpolator_mask;
  m.vertex.vertex.output_point_parameters =
      uint32_t((vertex.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               state.primitive_type == xg::xenos::PrimitiveType::kPointList);
  // User clip planes and vertex kill: not used by the game's draws so far.

  if (pixel) {
    m.pixel = SpirvShaderTranslator::Modification(translator_->GetDefaultPixelShaderModification(
        pixel->GetDynamicAddressableRegisterCount(state.sq_program_cntl.ps_num_reg)));
    m.pixel.pixel.interpolator_mask = interpolator_mask;
    m.pixel.pixel.interpolators_centroid =
        interpolator_mask & ~xg::xenos::GetInterpolatorSamplingPattern(
                                state.msaa_samples, state.sq_context_misc.sc_sample_cntl,
                                state.sq_interpolator_cntl.sampling_pattern);
    if (param_gen_pos < xg::xenos::kMaxInterpolators) {
      m.pixel.pixel.param_gen_enable = 1;
      m.pixel.pixel.param_gen_interpolator = param_gen_pos;
      m.pixel.pixel.param_gen_point =
          uint32_t(state.primitive_type == xg::xenos::PrimitiveType::kPointList);
    }
  }
  return m;
}

SpirvShader::Translation* ShaderLibrary::Translate(
    SpirvShader& shader, SpirvShaderTranslator::Modification modification) {
  bool is_new = false;
  SpirvShader::Translation* t = shader.GetOrCreateTranslation(modification.value, &is_new);
  if (is_new) translator_->TranslateAnalyzedShader(*t);
  return t->is_valid() ? t : nullptr;
}

}  // namespace tb2::native
