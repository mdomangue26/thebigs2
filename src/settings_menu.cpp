// thebigs2 - in-game settings menu (see settings_menu.h).

#include "settings_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "imgui_gamepad.h"
#include "settings.h"
#include "ui_theme.h"

namespace tb2 {
namespace {

namespace fs = std::filesystem;

// A value a setting can take: its TOML literal (empty: automatic, the line
// commented out) and how the menu shows it.
struct Choice {
  std::string literal;
  std::string label;
  const char* glyph = nullptr;  // controller button drawn before the label
};

enum class Kind { kChoice, kText };

struct Item {
  const char* key;
  const char* section;
  const char* label;
  const char* help;
  Kind kind = Kind::kChoice;
  std::vector<Choice> choices;  // kChoice
  std::string default_literal;  // when the file doesn't set it ("" = automatic)
  bool live = false;            // applies without a restart
  const char* glyph = nullptr;  // controller button drawn before the label
};

std::vector<Choice> OnOff() { return {{"true", "On"}, {"false", "Off"}}; }

// The settings of thebigs2.toml (its template documents them), with the
// defaults of this build and profile.
std::vector<Item> Items() {
  const bool deck = SteamDeckProfile();
  std::vector<Item> items = {
      {"fullscreen", "Display", "Fullscreen", "Fullscreen, or a window.", Kind::kChoice, OnOff(),
       "true"},
      {"fullscreen_exclusive", "Display", "Fullscreen kind",
       "Borderless uses the desktop's resolution and refresh rate; exclusive switches the "
       "monitor to the mode below.",
       Kind::kChoice, {{"false", "Borderless"}, {"true", "Exclusive"}}, "false"},
      {"display_resolution", "Display", "Resolution",
       "Window size, or the monitor mode in exclusive fullscreen.", Kind::kChoice,
       {{"\"desktop\"", "Desktop"},
        {"\"1280x720\"", "1280x720"},
        {"\"1280x800\"", "1280x800 (Steam Deck)"},
        {"\"1920x1080\"", "1920x1080"},
        {"\"2560x1440\"", "2560x1440"},
        {"\"3840x2160\"", "3840x2160"}},
       "\"desktop\""},
      {"display_refresh_rate", "Display", "Refresh rate",
       "Exclusive fullscreen only. Matches run at 30 fps (or 60): 60 or 120 Hz move the "
       "smoothest.",
       Kind::kChoice,
       {{"0", "Desktop rate"}, {"60", "60 Hz"}, {"120", "120 Hz"}, {"144", "144 Hz"},
        {"165", "165 Hz"}, {"240", "240 Hz"}},
       "0"},
      {"monitor", "Display", "Monitor", "Monitor to open on.", Kind::kChoice,
       {{"0", "System default"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}}, "0"},
      {"present_letterbox", "Display", "Keep 16:9",
       "Black bars on screens of other aspect ratios, instead of stretching.", Kind::kChoice,
       OnOff(), "true", true},
#if defined(_WIN32)
      {"gpu_backend", "Graphics", "Graphics API",
       "Vulkan (default; the settings here are tuned for it) or Direct3D 12 (less tested).",
       Kind::kChoice, {{"\"vulkan\"", "Vulkan"}, {"\"d3d12\"", "Direct3D 12"}}, "\"vulkan\""},
#endif
      {"resolution_scale", "Graphics", "Render scale",
       "Internal resolution. Higher is sharper and slower. 1x on GPUs without fragment "
       "shader interlock and with 60 fps.",
       Kind::kChoice,
       {{"1", "1x (1280x720)"}, {"2", "2x (2560x1440)"}, {"3", "3x (3840x2160)"},
        {"4", "4x (5120x2880)"}},
       deck ? "1" : "2"},
      {"render_target_path_vulkan", "Graphics", "EDRAM emulation",
       "FSI: accurate, needed at 2x and up with the game's MSAA. Host: faster. GPUs without "
       "fragment shader interlock always use host.",
       Kind::kChoice, {{"\"fsi\"", "FSI (accurate)"}, {"\"host\"", "Host render targets"}},
       deck ? "\"host\"" : "\"fsi\""},
      {"anisotropic_override", "Graphics", "Texture filtering",
       "Forcing anisotropic filtering sharpens the field and the stands at a distance.",
       Kind::kChoice,
       {{"-1", "Game's own"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"},
        {"5", "16x"}},
       deck ? "-1" : "3"},
      {"scene_msaa", "Graphics", "Scene MSAA (4x)",
       "The game's anti-aliasing on the 3D scene. Automatic: on at 1x, off at 2x and up and "
       "on the Steam Deck. Off saves about half the GPU emulation's CPU work.",
       Kind::kChoice, {{"", "Automatic"}, {"true", "On"}, {"false", "Off"}}, ""},
      {"present_effect", "Graphics", "Upscaling",
       "How the picture is scaled to the screen.", Kind::kChoice,
       {{"\"bilinear\"", "Bilinear"}, {"\"cas\"", "CAS (sharpened)"}, {"\"fsr\"", "FSR 1"}},
       "\"bilinear\"", true},
      {"frame_rate", "Graphics", "Frame rate in matches",
       "30 is the game's own. 60 is experimental: it needs a fast CPU and GPU (1x on the "
       "desktop); go back to 30 if it stutters.",
       Kind::kChoice, {{"30", "30 fps"}, {"60", "60 fps (experimental)"}}, "30", true},
      {"mnk_mode", "Controls", "Keyboard as player 1",
       "The keyboard plays as player 1's controller, together with the first pad.",
       Kind::kChoice, OnOff(), deck ? "false" : "true"},
      {"bind_quit", "Controls", "Quit key",
       "Key that quits the game (controller: hold Back + Start for 2 seconds).", Kind::kText, {},
       "\"F10\""},
      {"bind_settings_menu", "Controls", "Settings menu key",
       "Key that opens this menu (controller: hold Back + Y for a second).", Kind::kText, {},
       "\"F4\""},
  };
  // Controller buttons (SDK padbind_*): what each physical button presses in
  // the game.
  struct PadButton {
    const char* name;   // padbind_* value
    const char* label;  // in the menu
    const char* glyph;
  };
  static const PadButton kPad[] = {
      {"A", "A", "A"},          {"B", "B", "B"},
      {"X", "X", "X"},          {"Y", "Y", "Y"},
      {"LB", "LB", "LB"},       {"RB", "RB", "RB"},
      {"LT", "LT", "LT"},       {"RT", "RT", "RT"},
      {"Back", "Back (View)", "BACK"}, {"Start", "Start (Menu)", "START"},
      {"LS", "Left stick click", "LS"}, {"RS", "Right stick click", "RS"},
      {"Up", "D-pad up", "UP"},  {"Down", "D-pad down", "DOWN"},
      {"Left", "D-pad left", "LEFT"}, {"Right", "D-pad right", "RIGHT"}};
  std::vector<Choice> pad_choices;
  for (const PadButton& b : kPad) pad_choices.push_back({QuoteConfigString(b.name), b.label, b.glyph});
  pad_choices.push_back({"\"None\"", "Nothing"});
  static const char* const kPadKeys[] = {
      "padbind_a",  "padbind_b",     "padbind_x",  "padbind_y",  "padbind_lb",   "padbind_rb",
      "padbind_lt", "padbind_rt",    "padbind_back", "padbind_start", "padbind_ls", "padbind_rs",
      "padbind_up", "padbind_down",  "padbind_left", "padbind_right"};
  for (size_t i = 0; i < std::size(kPad); ++i) {
    Item item{kPadKeys[i], "Controller", kPad[i].label,
              "The game button this controller button presses (for every controller; the "
              "keyboard has its own keys). Applies after a restart.",
              Kind::kChoice, pad_choices, QuoteConfigString(kPad[i].name)};
    item.glyph = kPad[i].glyph;
    items.push_back(std::move(item));
  }
  static const std::tuple<const char*, const char*, const char*> kKeys[] = {
      {"keybind_a", "A", "A"}, {"keybind_b", "B", "B"}, {"keybind_x", "X", "X"},
      {"keybind_y", "Y", "Y"}, {"keybind_left_shoulder", "LB", "LB"},
      {"keybind_right_shoulder", "RB", "RB"}, {"keybind_left_trigger", "LT", "LT"},
      {"keybind_right_trigger", "RT", "RT"}, {"keybind_back", "Back (View)", "BACK"},
      {"keybind_start", "Start (Menu)", "START"},
      {"keybind_lstick_up", "Left stick up", "LS"}, {"keybind_lstick_down", "Left stick down", "LS"},
      {"keybind_lstick_left", "Left stick left", "LS"},
      {"keybind_lstick_right", "Left stick right", "LS"},
      {"keybind_lstick_press", "Left stick click", "LS"},
      {"keybind_rstick_up", "Right stick up", "RS"},
      {"keybind_rstick_down", "Right stick down", "RS"},
      {"keybind_rstick_left", "Right stick left", "RS"},
      {"keybind_rstick_right", "Right stick right", "RS"},
      {"keybind_rstick_press", "Right stick click", "RS"}, {"keybind_dpad_up", "D-pad up", "UP"},
      {"keybind_dpad_down", "D-pad down", "DOWN"}, {"keybind_dpad_left", "D-pad left", "LEFT"},
      {"keybind_dpad_right", "D-pad right", "RIGHT"}};
  for (const auto& [key, label, glyph] : kKeys) {
    Item item{key, "Keyboard", label,
              "The keyboard key(s) for this controller button: one key or several separated by "
              "commas (\"Q,I\"), with modifiers if needed (\"Shift+Up\"). Key names: see "
              "thebigs2.toml.",
              Kind::kText, {}, ""};
    item.glyph = glyph;
    items.push_back(std::move(item));
  }
  return items;
}

// "value" -> value (TOML basic string), anything else as it is.
std::string Unquote(const std::string& literal) {
  if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"') return literal;
  std::string out;
  for (size_t i = 1; i + 1 < literal.size(); ++i) {
    if (literal[i] == '\\' && i + 2 < literal.size()) ++i;
    out += literal[i];
  }
  return out;
}

struct State {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  rex::input::InputSystem* input = nullptr;
  fs::path config_path;
  std::function<void(std::function<void()>)> post_to_ui;
  std::function<void()> request_quit;
};
State g_state;

class SettingsMenu;
SettingsMenu* g_menu = nullptr;

class SettingsMenu final : public rex::ui::ImGuiDialog {
 public:
  explicit SettingsMenu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    saved_backend_flags_ = io.BackendFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // The game reads a neutral pad meanwhile (keyboard-as-pad included).
    if (g_state.input) g_state.input->AddUIInputBlocker();
    items_ = Items();
    Load();
  }

  ~SettingsMenu() override {
    if (g_state.input) g_state.input->RemoveUIInputBlocker();
    ImGuiIO& io = GetIO();
    io.ConfigFlags = saved_config_flags_;
    io.BackendFlags = saved_backend_flags_;
    if (g_menu == this) g_menu = nullptr;
  }

  void RequestClose() { Close(); }

 protected:
  // Drawn like the game's own menus (ui_theme.h): a slanted title band, the
  // sections as tabs (LB / RB), silver panel, the selected row on a gold band;
  // left / right changes a setting, as on the game's option screens.
  void OnDraw(ImGuiIO& io) override {
    namespace th = theme;
    gamepads_.Feed(io);
    const float s = std::clamp(io.DisplaySize.y / 720.0f, 0.75f, 4.0f);
    // A text field taking typing in the last frame keeps B / Escape / LB / RB.
    const bool typing = typing_was_active_;

    std::vector<std::string_view> tabs;
    for (const Item& item : items_)
      if (std::find(tabs.begin(), tabs.end(), item.section) == tabs.end())
        tabs.push_back(item.section);
    const int tab_count = int(tabs.size());
    if (!typing) {
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false))
        SelectTab((tab_ + tab_count - 1) % tab_count);
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
          ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
        SelectTab((tab_ + 1) % tab_count);
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##tb2_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    // The gold band marks the selected row: no ImGui focus frame on top.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(4, 8, 22, 205));

    // Layout (720p units times s).
    const float width = std::min(900.0f * s, io.DisplaySize.x * 0.94f);
    const float height = std::min(660.0f * s, io.DisplaySize.y * 0.96f);
    const float x0 = (io.DisplaySize.x - width) * 0.5f, x1 = x0 + width;
    const float top = (io.DisplaySize.y - height) * 0.5f, bottom = top + height;
    const float title_h = 56 * s, tabs_h = 40 * s, footer_h = 40 * s, help_h = 88 * s;

    // Title band.
    th::DrawTitleBand(dl, ImVec2(x0 - 14 * s, top), ImVec2(x0 + 400 * s, top + title_h), s);
    th::DrawLabel(dl, th::Face::kDisplay, 46 * s, ImVec2(x0 + 28 * s, top + 2 * s), th::kGold,
                 "SETTINGS");

    // Tabs, LB / RB at the ends.
    const float tabs_y = top + title_h + 8 * s;
    dl->AddRectFilled(ImVec2(x0, tabs_y), ImVec2(x1, tabs_y + tabs_h), IM_COL32(6, 8, 14, 225));
    dl->AddLine(ImVec2(x0, tabs_y), ImVec2(x1, tabs_y), th::kGold, std::max(1.0f, 2 * s));
    const float glyph_r = 13 * s;
    th::DrawPadButton(dl, ImVec2(x0 + 30 * s, tabs_y + tabs_h * 0.5f), glyph_r, "LB");
    th::DrawPadButton(dl, ImVec2(x1 - 30 * s, tabs_y + tabs_h * 0.5f), glyph_r, "RB");
    {
      const float tab_px = 27 * s;
      float total = 0;
      for (auto t : tabs) total += th::TextSize(th::Face::kDisplay, tab_px, th::Upper(t)).x;
      const float gap = std::max(24 * s, (width - 140 * s - total) / std::max(1, tab_count));
      float x = (x0 + x1) * 0.5f - (total + gap * (tab_count - 1)) * 0.5f;
      ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
      for (int t = 0; t < tab_count; ++t) {
        const std::string label = th::Upper(tabs[t]);
        const ImVec2 size = th::TextSize(th::Face::kDisplay, tab_px, label);
        const float y = tabs_y + (tabs_h - size.y) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x - 6 * s, tabs_y));
        ImGui::PushID(t);
        if (ImGui::InvisibleButton("##tab", ImVec2(size.x + 12 * s, tabs_h))) SelectTab(t);
        ImGui::PopID();
        const bool active = t == tab_;
        th::DrawLabel(dl, th::Face::kDisplay, tab_px, ImVec2(x, y),
                     active ? th::kGold : ImGui::IsItemHovered() ? th::kWhite : th::kSilverText, label);
        if (active)
          dl->AddRectFilled(ImVec2(x, tabs_y + tabs_h - 4 * s), ImVec2(x + size.x, tabs_y + tabs_h - 1 * s),
                            th::kGold);
        x += size.x + gap;
      }
      ImGui::PopItemFlag();
    }

    // Panel: rows, then the help text.
    const float panel_top = tabs_y + tabs_h + 6 * s;
    const float panel_bottom = bottom - footer_h - 8 * s;
    th::DrawSilverPanel(dl, ImVec2(x0, panel_top), ImVec2(x1, panel_bottom), s);
    const ImVec2 rows_min(x0 + 10 * s, panel_top + 10 * s);
    const ImVec2 rows_max(x1 - 10 * s, panel_bottom - help_h - 12 * s);
    const char* help = nullptr;
    ImGui::SetCursorScreenPos(rows_min);
    ImGui::BeginChild("##rows", ImVec2(rows_max.x - rows_min.x, rows_max.y - rows_min.y),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    {
      ImDrawList* rl = ImGui::GetWindowDrawList();
      const float row_h = 38 * s, row_gap = 3 * s, w = rows_max.x - rows_min.x;
      const float label_px = 25 * s, value_px = 25 * s;
      const float value_right = w - 34 * s, value_left = w * 0.50f;
      ImGuiID focused_row = 0;
      bool first = true;
      for (size_t i = 0; i < items_.size(); ++i) {
        Item& item = items_[i];
        if (std::string_view(item.section) != tabs[tab_]) continue;
        ImGui::PushID(int(i));
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const ImVec2 r1(r0.x + w, r0.y + row_h);
        const ImGuiID row_id = ImGui::GetID("##row");
        const bool selected = row_id == focused_row_;
        // The band before the row's widgets (a text field draws over it).
        if (selected) th::DrawGoldBand(rl, r0, r1);
        else if (ImGui::IsMouseHoveringRect(r0, r1)) rl->AddRectFilled(r0, r1, IM_COL32(0, 0, 0, 28));
        if (selected) {  // the game's baseball cursor
          const ImVec2 c(r0.x + row_h * 0.32f, (r0.y + r1.y) * 0.5f);
          rl->AddCircleFilled(c, row_h * 0.22f, IM_COL32(236, 236, 230, 255));
          rl->AddCircle(c, row_h * 0.22f, IM_COL32(120, 120, 120, 255), 0, 1.0f);
          rl->AddBezierQuadratic(ImVec2(c.x - row_h * 0.12f, c.y - row_h * 0.17f), ImVec2(c.x - row_h * 0.03f, c.y),
                                 ImVec2(c.x - row_h * 0.12f, c.y + row_h * 0.17f), IM_COL32(200, 40, 40, 255),
                                 std::max(1.0f, 1.5f * s));
          rl->AddBezierQuadratic(ImVec2(c.x + row_h * 0.12f, c.y - row_h * 0.17f), ImVec2(c.x + row_h * 0.03f, c.y),
                                 ImVec2(c.x + row_h * 0.12f, c.y + row_h * 0.17f), IM_COL32(200, 40, 40, 255),
                                 std::max(1.0f, 1.5f * s));
        }
        const ImU32 ink = th::kInk;
        const float label_y = r0.y + (row_h - th::TextSize(th::Face::kDisplay, label_px, "A").y) * 0.5f;
        const std::string label = th::Upper(item.label);
        float label_x = r0.x + row_h * 1.25f;  // after the band's dark start
        if (item.glyph) {
          const float gr = row_h * 0.36f, gw = th::PadButtonWidth(item.glyph, gr);
          th::DrawPadButton(rl, ImVec2(label_x + gw * 0.5f, (r0.y + r1.y) * 0.5f), gr, item.glyph);
          label_x += gw + 10 * s;
        }
        th::DrawLabel(rl, th::Face::kDisplay, label_px, ImVec2(label_x, label_y), ink, label);
        if (values_[i] != loaded_[i]) {  // changed, not saved yet
          const float lx = label_x + th::TextSize(th::Face::kDisplay, label_px, label).x + 10 * s;
          const char* tag = item.live ? "CHANGED" : "AFTER A RESTART";
          const ImVec2 ts = th::TextSize(th::Face::kText, 16 * s, tag);
          const float ty = (r0.y + r1.y) * 0.5f - ts.y * 0.5f - 1 * s;
          rl->AddRectFilled(ImVec2(lx, ty - 1 * s), ImVec2(lx + ts.x + 10 * s, ty + ts.y + 1 * s),
                            th::kNavy, 3 * s);
          th::DrawLabel(rl, th::Face::kText, 16 * s, ImVec2(lx + 5 * s, ty), th::kGold, tag);
        }
        if (first_frame_ && first) {  // start on the first setting
          ImGui::SetKeyboardFocusHere();
          first_frame_ = false;
        }
        first = false;
        bool focused = false;
        if (item.kind == Kind::kChoice) {
          const bool pressed =
              ImGui::InvisibleButton("##row", ImVec2(w, row_h), ImGuiButtonFlags_EnableNav);
          focused = ImGui::IsItemFocused();
          int step = 0;
          if (pressed)  // click on the left arrow: back, else forward
            step = ImGui::GetIO().MousePos.x < r0.x + value_left + 10 * s &&
                           ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                       ? -1
                       : 1;
          if (focused && !typing) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft))
              step = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight))
              step = 1;
          }
          const std::vector<Choice> choices = ChoicesOf(item, values_[i]);
          if (step) {
            size_t at = 0;
            for (size_t k = 0; k < choices.size(); ++k)
              if (choices[k].literal == values_[i]) at = k;
            values_[i] = choices[(at + choices.size() + step) % choices.size()].literal;
          }
          std::string value;
          const char* value_glyph = nullptr;
          for (const Choice& c : choices)
            if (c.literal == values_[i]) value = ChoiceLabel(item, c), value_glyph = c.glyph;
          ImVec2 vs = th::TextSize(th::Face::kText, value_px, value);
          const float vy = r0.y + (row_h - vs.y) * 0.5f;
          th::DrawLabel(rl, th::Face::kText, value_px, ImVec2(r0.x + value_right, vy), ink, value, 1.0f);
          if (value_glyph) {  // the button's icon before its name
            const float gr = row_h * 0.33f, gw = th::PadButtonWidth(value_glyph, gr);
            th::DrawPadButton(rl, ImVec2(r0.x + value_right - vs.x - 8 * s - gw * 0.5f, (r0.y + r1.y) * 0.5f),
                              gr, value_glyph);
            vs.x += gw + 8 * s;
          }
          // Arrows: on the selected row, around the value.
          const ImU32 arrow = selected ? th::kInk : IM_COL32(24, 26, 32, 90);
          const float ay = (r0.y + r1.y) * 0.5f, ah = 7 * s;
          const float lx = std::max(r0.x + value_left, r0.x + value_right - vs.x - 18 * s);
          const float rx = r0.x + value_right + 14 * s;
          rl->AddTriangleFilled(ImVec2(lx, ay - ah), ImVec2(lx, ay + ah), ImVec2(lx - ah * 1.2f, ay), arrow);
          rl->AddTriangleFilled(ImVec2(rx, ay - ah), ImVec2(rx, ay + ah), ImVec2(rx + ah * 1.2f, ay), arrow);
        } else {
          // A text field: A / Enter to type, Enter to finish.
          ImGui::SetCursorScreenPos(r0);
          ImGui::InvisibleButton("##row", ImVec2(1, row_h));  // the row's id, for the band
          ImGui::SetCursorScreenPos(ImVec2(r0.x + value_left, r0.y + 4 * s));
          ImFont* font = th::Font(th::Face::kText, value_px);
          ImGui::PushFont(font, font->LegacySize);
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.09f, 0.10f, 0.13f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.35f));
          ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.55f));
          ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1, 1, 1, 0.95f));
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                              ImVec2(8 * s, (row_h - 8 * s - font->LegacySize) * 0.5f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * s);
          ImGui::SetNextItemWidth(value_right + 18 * s - value_left);
          std::string text = Unquote(values_[i]);
          char buffer[256] = {};
          text.copy(buffer, sizeof(buffer) - 1);
          if (ImGui::InputText("##text", buffer, sizeof(buffer))) values_[i] = QuoteConfigString(buffer);
          focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
          ImGui::PopStyleVar(2);
          ImGui::PopStyleColor(4);
          ImGui::PopFont();
          ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y));
          ImGui::Dummy(ImVec2(w, 0));
        }
        if (focused) focused_row = row_id;
        if (focused || ImGui::IsMouseHoveringRect(r0, r1)) help = item.help;
        if (focused && focused_row != focused_row_) ImGui::SetScrollHereY(0.5f);
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y + row_gap));
        ImGui::PopID();
      }
      ImGui::Dummy(ImVec2(w, 0));
      if (focused_row) focused_row_ = focused_row;
    }
    ImGui::EndChild();

    // Help: what the selected setting does, and the last save.
    {
      const ImVec2 h0(x0 + 10 * s, panel_bottom - help_h - 4 * s), h1(x1 - 10 * s, panel_bottom - 8 * s);
      dl->AddRectFilled(h0, h1, IM_COL32(12, 18, 38, 238), 3 * s);
      ImFont* font = th::Font(th::Face::kText, 21 * s);
      const float wrap = h1.x - h0.x - 24 * s;
      const char* text = help ? help : "Pick a setting to see what it does.";
      dl->AddText(font, 21 * s, ImVec2(h0.x + 12 * s, h0.y + 8 * s), th::kWhite, text, nullptr, wrap);
      if (!message_.empty()) {  // under the help text
        const float help_bottom = h0.y + 8 * s + font->CalcTextSizeA(21 * s, FLT_MAX, wrap, text).y;
        dl->AddText(font, 19 * s, ImVec2(h0.x + 12 * s, std::max(help_bottom + 4 * s, h1.y - 27 * s)),
                    th::kGold, message_.c_str(), nullptr, wrap);
      }
    }

    // Button prompts (also clickable).
    const bool changed = Changed();
    if (!typing && changed &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
         // (the SDK's ImGui doesn't get modifier keys: ask SDL)
         ((SDL_GetModState() & SDL_KMOD_CTRL) && ImGui::IsKeyPressed(ImGuiKey_S, false))))
      Save();
    bool save_quit = !typing && changed && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
    bool close = !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    {
      struct Prompt {
        const char* button;
        const char* label;
        bool enabled;
        int action;  // 0 none, 1 save, 2 save and quit, 3 close
      };
      const Prompt prompts[] = {
          {"A", "CHANGE", true, 0},
          {"START", "SAVE", changed, 1},
          {"Y", "SAVE AND QUIT", changed, 2},
          {"B", changed ? "CLOSE WITHOUT SAVING" : "CLOSE", true, 3},
      };
      const float fy = bottom - footer_h, px = 21 * s, r = 13 * s;
      dl->AddRectFilledMultiColor(ImVec2(x0, fy), ImVec2(x1, bottom), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 0));
      float x = x1 - 16 * s;
      ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
      for (int k = int(std::size(prompts)) - 1; k >= 0; --k) {
        const Prompt& p = prompts[k];
        const float tw = th::TextSize(th::Face::kDisplay, px, p.label).x;
        const float gw = th::PadButtonWidth(p.button, r);
        const float x_start = x - tw - 8 * s - gw;
        const ImU32 color = p.enabled ? th::kWhite : IM_COL32(150, 154, 164, 160);
        th::DrawPadButton(dl, ImVec2(x_start + gw * 0.5f, fy + footer_h * 0.5f), r, p.button);
        th::DrawLabel(dl, th::Face::kDisplay, px,
                     ImVec2(x - tw, fy + (footer_h - th::TextSize(th::Face::kDisplay, px, "A").y) * 0.5f),
                     color, p.label);
        ImGui::SetCursorScreenPos(ImVec2(x_start, fy));
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##prompt", ImVec2(x - x_start, footer_h)) && p.enabled) {
          if (p.action == 1) Save();
          if (p.action == 2) save_quit = true;
          if (p.action == 3) close = true;
        }
        ImGui::PopID();
        x = x_start - 26 * s;
      }
      ImGui::PopItemFlag();
    }
    if (save_quit) {
      Save();
      if (g_state.request_quit) g_state.request_quit();
    }
    typing_was_active_ = ImGui::GetIO().WantTextInput;
    ImGui::PopStyleColor();
    ImGui::End();
    if (close) Close();
  }

 private:
  void Load() {
    const auto file = ReadConfigValues(g_state.config_path);
    loaded_.clear();
    for (const Item& item : items_) {
      const auto it = file.find(item.key);
      loaded_.push_back(it != file.end() ? it->second : item.default_literal);
    }
    values_ = loaded_;
  }

  bool Changed() const { return values_ != loaded_; }

  // The item's choices; a value the file has that isn't one of them stays
  // selectable.
  static std::vector<Choice> ChoicesOf(const Item& item, const std::string& value) {
    std::vector<Choice> choices = item.choices;
    bool found = false;
    for (const Choice& c : choices) found |= c.literal == value;
    if (!found) choices.push_back({value, Unquote(value) + " (from the file)"});
    return choices;
  }
  static std::string ChoiceLabel(const Item& item, const Choice& c) {
    return c.label + (c.literal == item.default_literal ? " (default)" : "");
  }

  void SelectTab(int tab) {
    if (tab == tab_) return;
    tab_ = tab;
    first_frame_ = true;  // focus its first setting
    focused_row_ = 0;
  }

  void Save() {
    std::vector<std::string> restart;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (values_[i] == loaded_[i]) continue;
      const Item& item = items_[i];
      // The default when it is automatic (commented out), else the value.
      SaveConfigValue(g_state.config_path, item.key,
                      values_[i].empty() ? std::nullopt : std::optional<std::string>(values_[i]));
      if (item.live && !values_[i].empty()) {
        rex::cvar::SetFlagByName(item.key, Unquote(values_[i]));
      } else if (!item.live) {
        restart.push_back(item.label);
      }
      REXLOG_INFO("Settings menu: {} = {}", item.key, values_[i].empty() ? "(automatic)" : values_[i]);
    }
    Load();
    message_ = "Saved to " + g_state.config_path.string() + ".";
    if (!restart.empty()) {
      message_ += " After a restart:";
      for (size_t i = 0; i < restart.size(); ++i) message_ += (i ? ", " : " ") + restart[i];
      message_ += ".";
    }
  }

  std::vector<Item> items_;
  std::vector<std::string> loaded_, values_;  // TOML literals ("" = automatic)
  std::string message_;
  bool first_frame_ = true;
  bool typing_was_active_ = false;
  int tab_ = 0;
  ImGuiID focused_row_ = 0;  // last frame's (the gold band is drawn before the row)
  ImGuiGamepads gamepads_;
  ImGuiConfigFlags saved_config_flags_ = 0;
  ImGuiBackendFlags saved_backend_flags_ = 0;
};

}  // namespace

void InitSettingsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                      std::filesystem::path config_path,
                      std::function<void(std::function<void()>)> post_to_ui,
                      std::function<void()> request_quit) {
  g_state = {drawer, input, std::move(config_path), std::move(post_to_ui),
             std::move(request_quit)};
}

void ToggleSettingsMenu() {
  if (!g_state.drawer || !g_state.post_to_ui) return;
  g_state.post_to_ui([] {
    if (g_menu) {
      g_menu->RequestClose();
    } else {
      g_menu = new SettingsMenu(g_state.drawer);  // deletes itself on close
    }
  });
}

}  // namespace tb2
