// Present pass, FXAA: the gamma-corrected frame (linear sampler) anti-aliased
// into the presenter's guest output image. FXAA 3.11 "quality" (Timothy
// Lottes, NVIDIA): local contrast finds edges, a search along the edge finds
// its ends, and the pixel is resampled across the edge by how far it is from
// the nearer end; plus a sub-pixel blend for single-pixel features.
#version 450
layout(set = 0, binding = 0) uniform texture2D source_texture;
layout(set = 0, binding = 1) uniform sampler linear_sampler;
#define source sampler2D(source_texture, linear_sampler)
layout(location = 0) out vec4 color;

const float kEdgeThreshold = 0.166;      // minimum local contrast, relative
const float kEdgeThresholdMin = 0.0833;  // and absolute (dark areas)
const float kSubpix = 0.75;              // sub-pixel aliasing removal
const int kSteps = 10;
const float kStep[kSteps] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float LumaAt(vec2 uv) { return Luma(textureLod(source, uv, 0.0).rgb); }
#define LumaOff(uv, o) Luma(textureLodOffset(source, uv, 0.0, o).rgb)

void main() {
  vec2 rcp = 1.0 / vec2(textureSize(source, 0));
  vec2 uv = gl_FragCoord.xy * rcp;
  vec3 rgbM = textureLod(source, uv, 0.0).rgb;
  float lumaM = Luma(rgbM);
  float lumaS = LumaOff(uv, ivec2(0, 1));
  float lumaE = LumaOff(uv, ivec2(1, 0));
  float lumaN = LumaOff(uv, ivec2(0, -1));
  float lumaW = LumaOff(uv, ivec2(-1, 0));
  float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
  float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
  float range = rangeMax - rangeMin;
  if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold)) {
    color = vec4(rgbM, 1.0);
    return;
  }
  float lumaNW = LumaOff(uv, ivec2(-1, -1));
  float lumaSE = LumaOff(uv, ivec2(1, 1));
  float lumaNE = LumaOff(uv, ivec2(1, -1));
  float lumaSW = LumaOff(uv, ivec2(-1, 1));

  // Edge direction.
  float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
  float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
  float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
  float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 +
                   abs(-2.0 * lumaE + lumaNESE);
  float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 +
                   abs(-2.0 * lumaN + lumaNWNE);
  bool horzSpan = edgeHorz >= edgeVert;
  float lengthSign = horzSpan ? rcp.y : rcp.x;
  if (!horzSpan) {
    lumaN = lumaW;
    lumaS = lumaE;
  }
  float subpixA = (lumaNS + lumaWE) * 2.0 + (lumaNWSW + lumaNESE);
  float subpixB = subpixA * (1.0 / 12.0) - lumaM;
  float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
  float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
  bool pairN = abs(gradientN) >= abs(gradientS);
  float gradient = max(abs(gradientN), abs(gradientS));
  if (pairN) lengthSign = -lengthSign;
  float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);

  // Search along the edge for its ends.
  vec2 posB = uv;
  vec2 offNP = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
  if (horzSpan) posB.y += lengthSign * 0.5;
  else posB.x += lengthSign * 0.5;
  if (!pairN) lumaNN = lumaSS;
  float gradientScaled = gradient * 0.25;
  float lumaMM = lumaM - lumaNN * 0.5;
  bool lumaMLTZero = lumaMM < 0.0;
  vec2 posN = posB - offNP * kStep[0], posP = posB + offNP * kStep[0];
  float lumaEndN = LumaAt(posN) - lumaNN * 0.5, lumaEndP = LumaAt(posP) - lumaNN * 0.5;
  bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
  for (int i = 1; i < kSteps && !(doneN && doneP); ++i) {
    if (!doneN) {
      posN -= offNP * kStep[i];
      lumaEndN = LumaAt(posN) - lumaNN * 0.5;
      doneN = abs(lumaEndN) >= gradientScaled;
    }
    if (!doneP) {
      posP += offNP * kStep[i];
      lumaEndP = LumaAt(posP) - lumaNN * 0.5;
      doneP = abs(lumaEndP) >= gradientScaled;
    }
  }

  // Resample across the edge.
  float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
  float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
  bool directionN = dstN < dstP;
  bool goodSpan = directionN ? ((lumaEndN < 0.0) != lumaMLTZero) : ((lumaEndP < 0.0) != lumaMLTZero);
  float pixelOffset = 0.5 - min(dstN, dstP) / (dstN + dstP);
  float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
  float offset = max(goodSpan ? pixelOffset : 0.0, subpixF * subpixF * kSubpix);
  vec2 pos = uv;
  if (horzSpan) pos.y += offset * lengthSign;
  else pos.x += offset * lengthSign;
  color = vec4(textureLod(source, pos, 0.0).rgb, 1.0);
}
