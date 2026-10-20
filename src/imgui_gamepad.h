// thebigs2 - ImGui gamepad navigation from SDL gamepads, for the game's own
// screens (first launch, settings menu): the SDK's ImGui gets keyboard and
// mouse only. Real controllers only - not the keyboard-as-controller mapping,
// which would press A along with Space.
#pragma once

#include <vector>

struct ImGuiIO;
struct SDL_Gamepad;

namespace tb2 {

class ImGuiGamepads {
 public:
  ImGuiGamepads();
  ~ImGuiGamepads();
  ImGuiGamepads(const ImGuiGamepads&) = delete;
  ImGuiGamepads& operator=(const ImGuiGamepads&) = delete;

  // Feeds this frame's buttons and left stick: A = activate, B = back,
  // D-pad / left stick = move, Start for the screen's own use. Buttons already held at the first call count
  // once released (the combo that opened a screen doesn't also act in it).
  void Feed(ImGuiIO& io);

 private:
  bool ok_ = false;
  bool armed_ = false;
  std::vector<SDL_Gamepad*> pads_;
};

}  // namespace tb2
