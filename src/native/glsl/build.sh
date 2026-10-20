#!/bin/sh
# Compiles the renderer's own shaders into C headers (SPIR-V words). Any
# glslangValidator works, e.g. RenderDoc's:
#   GLSLANG=~/Downloads/renderdoc_1.46/share/renderdoc/plugins/spirv/glslangValidator src/native/glsl/build.sh
set -e
cd "$(dirname "$0")"
G=${GLSLANG:-glslangValidator}
"$G" -V --target-env vulkan1.1 --vn present_vs -o present_vs.h present.vert
"$G" -V --target-env vulkan1.1 --vn resolve_ps -o resolve_ps.h resolve.frag
"$G" -V --target-env vulkan1.1 --vn resolve_ms_ps -o resolve_ms_ps.h resolve_ms.frag
"$G" -V --target-env vulkan1.1 --vn pack_depth_ps -o pack_depth_ps.h pack_depth.frag
"$G" -V --target-env vulkan1.1 --vn pack_depth_ms_ps -o pack_depth_ms_ps.h pack_depth_ms.frag
"$G" -V --target-env vulkan1.1 --vn present_gamma_table_ps -o present_gamma_table_ps.h present_gamma_table.frag
"$G" -V --target-env vulkan1.1 --vn present_fxaa_ps -o present_fxaa_ps.h present_fxaa.frag
