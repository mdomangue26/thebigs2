// Present pass (src/native/renderer.cpp): the front buffer through the
// guest's 256-entry gamma ramp (DC_LUT_30_COLOR: blue bits 0-9, green 10-19,
// red 20-29), as the SDK's apply_gamma_table shader does it, into the SDK
// presenter's guest output.
#version 450
layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1, std140) uniform Ramp {
  uvec4 entries[64];
} ramp;
layout(set = 0, binding = 2) uniform sampler point_sampler;
layout(location = 0) out vec4 color;

uint Entry(uint i) { return ramp.entries[i >> 2][i & 3u]; }

void main() {
  vec3 c = texelFetch(sampler2D(source, point_sampler), ivec2(gl_FragCoord.xy), 0).rgb;
  uvec3 i = min(uvec3(c * 255.0 + 0.5), uvec3(255u));
  color = vec4(float((Entry(i.r) >> 20) & 1023u), float((Entry(i.g) >> 10) & 1023u),
               float(Entry(i.b) & 1023u), 1023.0) / 1023.0;
}
