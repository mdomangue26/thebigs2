// MSAA resolve (src/native/renderer.cpp): as resolve.frag, with the samples
// averaged (color) or sample 0 taken (depth).
#version 450
layout(constant_id = 0) const int kSamples = 4;
layout(constant_id = 1) const bool kFirstSample = false;
layout(set = 0, binding = 0) uniform texture2DMS source;
layout(set = 0, binding = 1) uniform sampler point_sampler;
layout(push_constant) uniform Constants { ivec2 offset; } pc;
layout(location = 0) out vec4 color;
void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy) + pc.offset;
  vec4 c = texelFetch(sampler2DMS(source, point_sampler), p, 0);
  if (!kFirstSample) {
    for (int i = 1; i < kSamples; ++i) c += texelFetch(sampler2DMS(source, point_sampler), p, i);
    c /= float(kSamples);
  }
  color = c;
}
