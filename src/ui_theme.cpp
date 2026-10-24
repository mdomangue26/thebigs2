// thebigs2 - overlay look (see ui_theme.h).

#include "ui_theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>

#include <rex/ui/style.h>

namespace tb2::theme {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
// Barlow Condensed (SIL OFL 1.1, assets/fonts/LICENSE-BarlowCondensed.txt).
const unsigned char kDisplayTtf[] = {
#embed "../assets/fonts/BarlowCondensed-BlackItalic.ttf"
};
const unsigned char kTextTtf[] = {
#embed "../assets/fonts/BarlowCondensed-SemiBold.ttf"
};
#pragma clang diagnostic pop

// Baked sizes: enough for 720p (Steam Deck 800) up to 2160p windows.
constexpr float kSizes[] = {16, 19, 22, 26, 31, 37, 44, 52, 62, 74, 88, 104};
constexpr int kSizeCount = int(std::size(kSizes));
ImFont* g_fonts[2][kSizeCount] = {};

}  // namespace

void AddFonts(ImFontAtlas* atlas) {
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
  for (int face = 0; face < 2; ++face) {
    const unsigned char* data = face ? kDisplayTtf : kTextTtf;
    const int bytes = face ? int(sizeof(kDisplayTtf)) : int(sizeof(kTextTtf));
    for (int i = 0; i < kSizeCount; ++i) {
      ImFontConfig cfg;
      cfg.FontDataOwnedByAtlas = false;  // static data
      cfg.OversampleH = 2;
      cfg.OversampleV = 1;
      g_fonts[face][i] = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), bytes,
                                                     kSizes[i], &cfg, kRanges);
    }
  }
  // The SDK's windows (F3, console) in the text face instead of its 10 px
  // ProggyTiny.
  if (ImGui::GetCurrentContext()) ImGui::GetIO().FontDefault = g_fonts[0][2];
}

ImFont* Font(Face face, float pixels) {
  ImFont* const* fonts = g_fonts[face == Face::kDisplay];
  int best = 0;
  for (int i = 1; i < kSizeCount; ++i)
    if (std::fabs(kSizes[i] - pixels) < std::fabs(kSizes[best] - pixels)) best = i;
  return fonts[best] ? fonts[best] : ImGui::GetFont();
}

std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

void ConfigureStyle(ImGuiStyle& style, rex::ui::Style& ui_style) {
  style.WindowRounding = 0.0f;
  style.ChildRounding = 0.0f;
  style.FrameRounding = 2.0f;
  style.PopupRounding = 0.0f;
  style.GrabRounding = 2.0f;
  style.ScrollbarRounding = 2.0f;
  style.TabRounding = 0.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(10, 8);
  style.FramePadding = ImVec2(6, 3);
  style.ItemSpacing = ImVec2(8, 4);
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

  const ImVec4 navy(0.035f, 0.063f, 0.15f, 0.92f);
  const ImVec4 navy_light(0.10f, 0.15f, 0.30f, 1.0f);
  const ImVec4 navy_lighter(0.17f, 0.23f, 0.42f, 1.0f);
  const ImVec4 gold(0.95f, 0.73f, 0.20f, 1.0f);
  const ImVec4 gold_dim(0.70f, 0.52f, 0.12f, 1.0f);
  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = ImVec4(0.96f, 0.96f, 0.97f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.64f, 0.73f, 1.0f);
  c[ImGuiCol_WindowBg] = navy;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(0.035f, 0.063f, 0.15f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(gold.x, gold.y, gold.z, 0.65f);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = navy_light;
  c[ImGuiCol_FrameBgHovered] = navy_lighter;
  c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.30f, 0.50f, 1.0f);
  c[ImGuiCol_TitleBg] = ImVec4(0.02f, 0.03f, 0.08f, 0.95f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.02f, 0.03f, 0.08f, 0.98f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.02f, 0.03f, 0.08f, 0.80f);
  c[ImGuiCol_MenuBarBg] = navy_light;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
  c[ImGuiCol_ScrollbarGrab] = navy_lighter;
  c[ImGuiCol_ScrollbarGrabHovered] = gold_dim;
  c[ImGuiCol_ScrollbarGrabActive] = gold;
  c[ImGuiCol_CheckMark] = gold;
  c[ImGuiCol_SliderGrab] = gold_dim;
  c[ImGuiCol_SliderGrabActive] = gold;
  c[ImGuiCol_Button] = navy_light;
  c[ImGuiCol_ButtonHovered] = navy_lighter;
  c[ImGuiCol_ButtonActive] = gold_dim;
  c[ImGuiCol_Header] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
  c[ImGuiCol_HeaderHovered] = ImVec4(gold.x, gold.y, gold.z, 0.65f);
  c[ImGuiCol_HeaderActive] = gold;
  c[ImGuiCol_Separator] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
  c[ImGuiCol_SeparatorHovered] = gold_dim;
  c[ImGuiCol_SeparatorActive] = gold;
  c[ImGuiCol_ResizeGrip] = ImVec4(gold.x, gold.y, gold.z, 0.25f);
  c[ImGuiCol_ResizeGripHovered] = gold_dim;
  c[ImGuiCol_ResizeGripActive] = gold;
  c[ImGuiCol_Tab] = navy_light;
  c[ImGuiCol_TabHovered] = navy_lighter;
  c[ImGuiCol_TabSelected] = gold_dim;
  c[ImGuiCol_TextSelectedBg] = ImVec4(gold.x, gold.y, gold.z, 0.40f);
  c[ImGuiCol_NavCursor] = gold;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.04f, 0.10f, 0.60f);

  ui_style.debug.muted_text = ImVec4(0.75f, 0.78f, 0.86f, 0.80f);
  ui_style.toast.title = gold;
}

void DrawSilverPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale) {
  const float rim = std::max(1.0f, 2.0f * scale);
  dl->AddRectFilled(ImVec2(min.x - rim, min.y - rim), ImVec2(max.x + rim, max.y + rim),
                    IM_COL32(20, 22, 28, 235));
  dl->AddRectFilledMultiColor(min, max, IM_COL32(232, 234, 238, 250), IM_COL32(232, 234, 238, 250),
                              IM_COL32(168, 172, 180, 250), IM_COL32(168, 172, 180, 250));
}

void DrawGoldBand(ImDrawList* dl, ImVec2 min, ImVec2 max) {
  const float h = max.y - min.y;
  const float fade = std::min(h * 0.55f, (max.x - min.x) * 0.3f);
  const ImU32 black = IM_COL32(10, 10, 12, 240);
  dl->AddRectFilled(min, ImVec2(min.x + h * 0.6f, max.y), black);
  dl->AddRectFilledMultiColor(ImVec2(min.x + h * 0.6f, min.y), ImVec2(min.x + h * 0.6f + fade, max.y),
                              black, kGold, kGold, black);
  dl->AddRectFilledMultiColor(ImVec2(min.x + h * 0.6f + fade, min.y), max, kGoldLight, kGoldLight,
                              kGold, kGold);
}

void DrawTitleBand(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale) {
  const float slant = (max.y - min.y) * 0.45f;
  const ImVec2 p[4] = {ImVec2(min.x + slant, min.y), ImVec2(max.x, min.y), ImVec2(max.x - slant, max.y),
                       ImVec2(min.x, max.y)};
  dl->AddConvexPolyFilled(p, 4, IM_COL32(6, 8, 14, 235));
  const float line = std::max(1.5f, 3.0f * scale);
  dl->AddLine(ImVec2(p[3].x, p[3].y - line * 0.5f), ImVec2(p[2].x, p[2].y - line * 0.5f), kGold, line);
  dl->AddLine(p[0], ImVec2(p[0].x + (max.x - min.x) * 0.35f, p[0].y), kGold, line * 0.6f);
}

namespace {

bool Is(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

ImU32 Shade(ImU32 color, float f) {  // f < 1 darker, > 1 lighter
  auto ch = [&](int shift) {
    const float v = float((color >> shift) & 0xFF);
    return uint32_t(std::clamp(f < 1 ? v * f : v + (255 - v) * (f - 1), 0.0f, 255.0f)) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) |
         (color & IM_COL32_A_MASK);
}

constexpr ImU32 kKeyFill = IM_COL32(48, 51, 58, 255);  // dark grey (bumpers, Menu, View, sticks)
constexpr ImU32 kKeyRim = IM_COL32(10, 10, 12, 255);

// A rounded polygon: the bumper (flat bottom, sloped top) or trigger shape.
void Shape(ImDrawList* dl, const ImVec2* p, int n, ImU32 fill, ImU32 rim, float rim_w) {
  dl->AddConvexPolyFilled(p, n, fill);
  dl->AddPolyline(p, n, rim, ImDrawFlags_Closed, rim_w);
}

}  // namespace

float PadButtonWidth(std::string_view b, float r) {
  if (Is(b, "LB") || Is(b, "RB")) return r * 3.3f;
  if (Is(b, "LT") || Is(b, "RT")) return r * 2.3f;
  return r * 2.0f;
}

void DrawPadButton(ImDrawList* dl, ImVec2 c, float r, std::string_view b) {
  const float rim_w = std::max(1.0f, r * 0.11f);
  const ImU32 white = IM_COL32(250, 250, 250, 255);
  // Face buttons: glossy colored domes with a white letter (the game's prompts).
  const struct {
    const char* name;
    ImU32 color;
  } faces[] = {{"A", IM_COL32(92, 176, 34, 255)},
               {"B", IM_COL32(222, 38, 30, 255)},
               {"X", IM_COL32(26, 110, 214, 255)},
               {"Y", IM_COL32(240, 182, 12, 255)}};
  for (const auto& f : faces) {
    if (!Is(b, f.name)) continue;
    dl->AddCircleFilled(c, r, kKeyRim);
    dl->AddCircleFilled(c, r - rim_w, Shade(f.color, 0.72f));
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.06f), r - rim_w * 1.9f, f.color);
    // Highlight: a lighter ellipse over the top half.
    dl->PathEllipticalArcTo(ImVec2(c.x, c.y - r * 0.32f), ImVec2(r * 0.62f, r * 0.42f), 0.0f, 0.0f,
                            6.2831853f);
    dl->PathFillConvex(Shade(f.color, 1.38f) & 0x8FFFFFFF);
    const float px = r * 1.45f;
    const ImVec2 ts = TextSize(Face::kDisplay, px, f.name);
    const ImVec2 tp(c.x - ts.x * 0.5f + r * 0.04f, c.y - ts.y * 0.5f);
    DrawLabel(dl, Face::kDisplay, px, ImVec2(tp.x + r * 0.06f, tp.y + r * 0.08f), IM_COL32(0, 0, 0, 120),
             f.name);
    DrawLabel(dl, Face::kDisplay, px, tp, white, f.name);
    return;
  }
  if (Is(b, "LB") || Is(b, "RB")) {  // bumper: flat bottom, top sloping down to the outside
    const float w = PadButtonWidth(b, r) * 0.5f, left = Is(b, "LB") ? 1.0f : -1.0f;
    const ImVec2 p[] = {ImVec2(c.x - w, c.y + r * 0.72f), ImVec2(c.x + w, c.y + r * 0.72f),
                        ImVec2(c.x + w, c.y - r * (left > 0 ? 0.55f : 0.15f)),
                        ImVec2(c.x + w - r * 0.35f, c.y - r * 0.75f),
                        ImVec2(c.x - w + r * 0.35f, c.y - r * 0.75f),
                        ImVec2(c.x - w, c.y - r * (left > 0 ? 0.15f : 0.55f))};
    Shape(dl, p, 6, kKeyFill, kKeyRim, rim_w);
    const float px = r * 1.15f;
    DrawLabel(dl, Face::kDisplay, px, ImVec2(c.x, c.y - TextSize(Face::kDisplay, px, b).y * 0.47f), white,
             Is(b, "LB") ? "LB" : "RB", 0.5f);
    return;
  }
  if (Is(b, "LT") || Is(b, "RT")) {  // trigger: tall, rounded top, narrower bottom
    const float w = PadButtonWidth(b, r) * 0.5f;
    const ImVec2 p[] = {ImVec2(c.x - w * 0.72f, c.y + r), ImVec2(c.x + w * 0.72f, c.y + r),
                        ImVec2(c.x + w, c.y - r * 0.35f), ImVec2(c.x + w * 0.65f, c.y - r),
                        ImVec2(c.x - w * 0.65f, c.y - r), ImVec2(c.x - w, c.y - r * 0.35f)};
    Shape(dl, p, 6, kKeyFill, kKeyRim, rim_w);
    const float px = r * 1.1f;
    DrawLabel(dl, Face::kDisplay, px, ImVec2(c.x, c.y - TextSize(Face::kDisplay, px, b).y * 0.5f), white,
             Is(b, "LT") ? "LT" : "RT", 0.5f);
    return;
  }
  // Round dark keys: Menu (Start), View (Back), sticks, d-pad.
  dl->AddCircleFilled(c, r, kKeyRim);
  dl->AddCircleFilled(c, r - rim_w, kKeyFill);
  const float lw = std::max(1.0f, r * 0.14f);
  if (Is(b, "START") || Is(b, "MENU")) {  // three lines
    for (int i = -1; i <= 1; ++i)
      dl->AddLine(ImVec2(c.x - r * 0.42f, c.y + i * r * 0.3f), ImVec2(c.x + r * 0.42f, c.y + i * r * 0.3f),
                  white, lw);
  } else if (Is(b, "BACK") || Is(b, "VIEW")) {  // two overlapping windows
    const float q = r * 0.3f;
    dl->AddRect(ImVec2(c.x - q * 1.45f, c.y - q * 1.1f), ImVec2(c.x + q * 0.55f, c.y + q * 0.45f), white,
                0.0f, 0, lw * 0.8f);
    dl->AddRectFilled(ImVec2(c.x - q * 0.55f, c.y - q * 0.45f), ImVec2(c.x + q * 1.45f, c.y + q * 1.1f),
                      white);
  } else if (Is(b, "LS") || Is(b, "RS")) {  // stick: ring and its letter
    dl->AddCircle(c, r * 0.62f, IM_COL32(170, 174, 182, 255), 0, lw);
    const char* letter = Is(b, "LS") ? "L" : "R";
    const float px = r * 1.15f;
    DrawLabel(dl, Face::kDisplay, px, ImVec2(c.x, c.y - TextSize(Face::kDisplay, px, letter).y * 0.5f), white,
             letter, 0.5f);
  } else if (Is(b, "UP") || Is(b, "DOWN") || Is(b, "LEFT") || Is(b, "RIGHT") || Is(b, "DPAD")) {
    const float a = r * 0.62f, t = r * 0.2f;  // arm length, half width
    const ImU32 dim = IM_COL32(120, 124, 132, 255);
    dl->AddRectFilled(ImVec2(c.x - t, c.y - a), ImVec2(c.x + t, c.y + a), dim);
    dl->AddRectFilled(ImVec2(c.x - a, c.y - t), ImVec2(c.x + a, c.y + t), dim);
    if (Is(b, "UP")) dl->AddRectFilled(ImVec2(c.x - t, c.y - a), ImVec2(c.x + t, c.y - t), white);
    if (Is(b, "DOWN")) dl->AddRectFilled(ImVec2(c.x - t, c.y + t), ImVec2(c.x + t, c.y + a), white);
    if (Is(b, "LEFT")) dl->AddRectFilled(ImVec2(c.x - a, c.y - t), ImVec2(c.x - t, c.y + t), white);
    if (Is(b, "RIGHT")) dl->AddRectFilled(ImVec2(c.x + t, c.y - t), ImVec2(c.x + a, c.y + t), white);
  } else {  // anything else: its name
    const float px = r * 0.9f;
    DrawLabel(dl, Face::kDisplay, px, ImVec2(c.x, c.y - TextSize(Face::kDisplay, px, b).y * 0.5f), white, b,
             0.5f);
  }
}

ImVec2 TextSize(Face face, float pixels, std::string_view text) {
  ImFont* font = Font(face, pixels);
  return font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
}

void DrawLabel(ImDrawList* dl, Face face, float pixels, ImVec2 pos, ImU32 color,
              std::string_view text, float align_x) {
  ImFont* font = Font(face, pixels);
  if (align_x != 0.0f) pos.x -= TextSize(face, pixels, text).x * align_x;
  dl->AddText(font, pixels, ImVec2(std::floor(pos.x), std::floor(pos.y)), color, text.data(),
              text.data() + text.size());
}

}  // namespace tb2::theme
