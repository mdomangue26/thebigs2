// Resolve (src/native/renderer.cpp): copies a render target region into a
// texture drawn as a color attachment, which also converts the format.
#version 450
layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform sampler point_sampler;
// Source texel = destination pixel + offset.
layout(push_constant) uniform Constants { ivec2 offset; } pc;
layout(location = 0) out vec4 color;
void main() {
  color = texelFetch(sampler2D(source, point_sampler), ivec2(gl_FragCoord.xy) + pc.offset, 0);
}
