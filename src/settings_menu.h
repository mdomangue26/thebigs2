// thebigs2 - in-game settings menu (F4, hold Back + Y on a controller, or
// RECOMP SETTINGS on the main menu: main_menu_entry.cpp).
//
// Edits thebigs2.toml, the same file as by hand: shows the values the file
// sets (or the defaults), and Save writes the changed ones into their lines,
// keeping the comments. A few settings apply at once (frame rate, upscaling
// effect, letterbox); the rest on the next start - "Save and quit" closes the
// game cleanly for that. Settings given on the command line still win. While
// the menu is open the game sees a neutral controller.

#pragma once

#include <filesystem>
#include <functional>

namespace rex::input {
class InputSystem;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace tb2 {

// post_to_ui: runs a callback on the UI thread; request_quit: the clean quit.
void InitSettingsMenu(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                      std::filesystem::path config_path,
                      std::function<void(std::function<void()>)> post_to_ui,
                      std::function<void()> request_quit);

// Opens the menu, or closes it (discarding unsaved changes). Any thread.
void ToggleSettingsMenu();
// Opens the menu if it isn't open (the main menu's RECOMP SETTINGS). Any thread.
void OpenSettingsMenu();

}  // namespace tb2
