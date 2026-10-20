// Depth resolves in the console's memory format (src/native/renderer.cpp):
// (depth24 << 8) | stencil per texel, depth as 24-bit unorm or 20e4 float (the
// EDRAM depth format), for textures that read a depth resolve's memory as
// color. Drawn like resolve.frag into an R32_UINT texture.
#version 450
layout(set = 0, binding = 0) uniform texture2D depth_source;
layout(set = 0, binding = 1) uniform utexture2D stencil_source;
layout(set = 0, binding = 2) uniform sampler point_sampler;
layout(push_constant) uniform Constants {
  ivec2 offset;  // source texel = destination pixel + offset
  uint float24;
} pc;
layout(location = 0) out uint packed;

// xenos::Float32To20e4, rounding to nearest even.
uint Float32To20e4(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (u < 0x38800000u) {
    uint shift = min(113u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
  } else {
    u += 0xC8000000u;
  }
  u += 3u + ((u >> 3) & 1u);
  return (u >> 3) & 0xFFFFFFu;
}

void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy) + pc.offset;
  float d = texelFetch(sampler2D(depth_source, point_sampler), p, 0).r;
  uint s = texelFetch(usampler2D(stencil_source, point_sampler), p, 0).r & 0xFFu;
  uint z = pc.float24 != 0u ? Float32To20e4(d) : uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5);
  packed = (z << 8) | s;
}
