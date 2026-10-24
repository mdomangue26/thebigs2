// thebigs2 - look of our own overlays (settings menu) and the SDK's (F3 debug
// window, console) to match the game's menus: navy backgrounds, silver
// panels, gold bands on the selected item, heavy italic capitals.
//
// Fonts: Barlow Condensed (SIL OFL, assets/fonts/LICENSE-BarlowCondensed.txt)
// Black Italic for titles and menu items, SemiBold for text. The SDK's ImGui
// renderer has a fixed font atlas, so each is baked at a few sizes; Font()
// picks the nearest.

#pragma once

#include <string>
#include <string_view>

#include <imgui.h>

namespace rex::ui {
struct Style;
}

namespace tb2::theme {

enum class Face { kText, kDisplay };

// From ReXApp::OnConfigureFonts: bakes the fonts and makes the text face the
// default font (F3 window, console).
void AddFonts(ImFontAtlas* atlas);
// From ReXApp::OnConfigureStyle: colors and shapes of every ImGui window.
void ConfigureStyle(ImGuiStyle& style, rex::ui::Style& ui_style);

ImFont* Font(Face face, float pixels);
// The text in capitals (the game's menus have no lowercase).
std::string Upper(std::string_view text);

// Colors (ImU32, for draw lists).
inline constexpr ImU32 kNavy = IM_COL32(9, 16, 38, 255);
inline constexpr ImU32 kGold = IM_COL32(242, 186, 52, 255);
inline constexpr ImU32 kGoldLight = IM_COL32(252, 214, 96, 255);
inline constexpr ImU32 kInk = IM_COL32(24, 26, 32, 255);       // text on silver / gold
inline constexpr ImU32 kSilverText = IM_COL32(205, 210, 220, 255);
inline constexpr ImU32 kWhite = IM_COL32(245, 246, 248, 255);

// The game's light grey panel (dialog boxes): vertical gradient, dark rim.
void DrawSilverPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale);
// The selection band: black at the left fading into gold.
void DrawGoldBand(ImDrawList* dl, ImVec2 min, ImVec2 max);
// A dark slanted band with gold edges (titles, like "PRESS START BUTTON").
void DrawTitleBand(ImDrawList* dl, ImVec2 min, ImVec2 max, float scale);
// A controller button glyph, styled like the game's Xbox prompts: "A", "B",
// "X", "Y", "LB", "RB", "LT", "RT", "START" (Menu), "BACK" (View), "LS",
// "RS", "UP", "DOWN", "LEFT", "RIGHT" (d-pad). radius: half its height.
void DrawPadButton(ImDrawList* dl, ImVec2 center, float radius, std::string_view button);
float PadButtonWidth(std::string_view button, float radius);
// Text with its top left at pos (align: 0 left, 0.5 centered, 1 right of pos.x).
void DrawLabel(ImDrawList* dl, Face face, float pixels, ImVec2 pos, ImU32 color,
              std::string_view text, float align_x = 0.0f);
ImVec2 TextSize(Face face, float pixels, std::string_view text);

}  // namespace tb2::theme
