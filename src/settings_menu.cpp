// thebigs2 - in-game settings menu (see settings_menu.h).

#include "settings_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
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

namespace tb2 {
namespace {

namespace fs = std::filesystem;

// A value a setting can take: its TOML literal (empty: automatic, the line
// commented out) and how the menu shows it.
struct Choice {
  std::string literal;
  std::string label;
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
      {"frame_rate", "Frame rate", "Frame rate in matches",
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
  static const std::pair<const char*, const char*> kKeys[] = {
      {"keybind_a", "A"}, {"keybind_b", "B"}, {"keybind_x", "X"}, {"keybind_y", "Y"},
      {"keybind_left_shoulder", "LB"}, {"keybind_right_shoulder", "RB"},
      {"keybind_left_trigger", "LT"}, {"keybind_right_trigger", "RT"},
      {"keybind_back", "Back"}, {"keybind_start", "Start"},
      {"keybind_lstick_up", "Left stick up"}, {"keybind_lstick_down", "Left stick down"},
      {"keybind_lstick_left", "Left stick left"}, {"keybind_lstick_right", "Left stick right"},
      {"keybind_lstick_press", "Left stick press"}, {"keybind_rstick_up", "Right stick up"},
      {"keybind_rstick_down", "Right stick down"}, {"keybind_rstick_left", "Right stick left"},
      {"keybind_rstick_right", "Right stick right"},
      {"keybind_rstick_press", "Right stick press"}, {"keybind_dpad_up", "D-pad up"},
      {"keybind_dpad_down", "D-pad down"}, {"keybind_dpad_left", "D-pad left"},
      {"keybind_dpad_right", "D-pad right"}};
  for (const auto& [key, label] : kKeys)
    items.push_back({key, "Keyboard keys", label,
                     "One key or several separated by commas (\"Q,I\"), with modifiers if "
                     "needed (\"Shift+Up\"). Key names: see thebigs2.toml.",
                     Kind::kText, {}, ""});
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
  void OnDraw(ImGuiIO& io) override {
    gamepads_.Feed(io);
    // A combo's list open in the last frame takes B / Escape for itself (ImGui
    // closes it before this runs).
    const bool popup_open =
        popup_was_open_ || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    const ImVec2 size(std::min(io.DisplaySize.x * 0.9f, 900.0f), io.DisplaySize.y * 0.85f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    const float footer = ImGui::GetFrameHeightWithSpacing() * 5.5f;
    ImGui::BeginChild("items", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
    const char* help = nullptr;
    const char* section = "";
    bool keys_open = false;
    for (size_t i = 0; i < items_.size(); ++i) {
      Item& item = items_[i];
      if (std::string_view(item.section) != section) {
        section = item.section;
        if (std::string_view(section) == "Keyboard keys") {
          keys_open = ImGui::CollapsingHeader("Keyboard keys");
        } else {
          ImGui::SeparatorText(section);
        }
      }
      if (std::string_view(section) == "Keyboard keys" && !keys_open) continue;
      ImGui::PushID(int(i));
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(item.label);
      if (values_[i] != loaded_[i]) {
        ImGui::SameLine();
        ImGui::TextDisabled(item.live ? "(changed)" : "(changed, after a restart)");
      }
      ImGui::SameLine(size.x * 0.42f);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (first_frame_) {  // start on the first setting
        ImGui::SetKeyboardFocusHere();
        first_frame_ = false;
      }
      if (item.kind == Kind::kChoice) {
        DrawChoice(item, values_[i]);
      } else {
        std::string text = Unquote(values_[i]);
        char buffer[256] = {};
        text.copy(buffer, sizeof(buffer) - 1);
        if (ImGui::InputText("##text", buffer, sizeof(buffer)))
          values_[i] = QuoteConfigString(buffer);
      }
      if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) help = item.help;
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextWrapped("%s", help ? help : "Select a setting to see what it does.");
    if (!message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
      ImGui::TextWrapped("%s", message_.c_str());
      ImGui::PopStyleColor();
    }
    const bool changed = Changed();
    // Start (controller) or Ctrl+S saves from anywhere in the menu.
    if (changed && !popup_open &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
         // (the SDK's ImGui doesn't get modifier keys: ask SDL)
         ((SDL_GetModState() & SDL_KMOD_CTRL) && ImGui::IsKeyPressed(ImGuiKey_S, false))))
      Save();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Save and quit")) {
      Save();
      if (g_state.request_quit) g_state.request_quit();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(changed ? "Close without saving" : "Close")) Close();
    ImGui::SameLine();
    ImGui::TextDisabled("Start / Ctrl+S: save   F4 / B: close");
    // B (or Escape) closes, unless it's closing a combo's list first.
    if (!popup_open &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      Close();
    popup_was_open_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::End();
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

  void DrawChoice(const Item& item, std::string& value) {
    // A value the file has that isn't one of the choices stays selectable.
    std::vector<Choice> choices = item.choices;
    bool found = false;
    for (const Choice& c : choices) found |= c.literal == value;
    if (!found) choices.push_back({value, Unquote(value) + " (from the file)"});
    std::string preview;
    for (const Choice& c : choices)
      if (c.literal == value)
        preview = c.label + (c.literal == item.default_literal ? " (default)" : "");
    if (ImGui::BeginCombo("##choice", preview.c_str())) {
      for (const Choice& c : choices) {
        const bool selected = c.literal == value;
        const std::string label =
            c.label + (c.literal == item.default_literal ? " (default)" : "");
        if (ImGui::Selectable(label.c_str(), selected)) value = c.literal;
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
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
  bool popup_was_open_ = false;
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
