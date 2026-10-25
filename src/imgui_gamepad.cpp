// thebigs2 - ImGui gamepad navigation from SDL gamepads (see imgui_gamepad.h).

#include "imgui_gamepad.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tb2 {

ImGuiGamepads::ImGuiGamepads() : ok_(SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {}

ImGuiGamepads::~ImGuiGamepads() {
  for (SDL_Gamepad* pad : pads_) SDL_CloseGamepad(pad);
  if (ok_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void ImGuiGamepads::Feed(ImGuiIO& io) {
  if (!ok_) return;
  std::erase_if(pads_, [](SDL_Gamepad* pad) {
    if (SDL_GamepadConnected(pad)) return false;
    SDL_CloseGamepad(pad);
    return true;
  });
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      // Our own reference even when the SDK's input driver has the pad open
      // already (SDL counts opens; SDL_GetGamepadFromID would return its).
      if (std::none_of(pads_.begin(), pads_.end(),
                       [id = ids[i]](SDL_Gamepad* pad) { return SDL_GetGamepadID(pad) == id; })) {
        if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) pads_.push_back(pad);
      }
    }
    SDL_free(ids);
  }
  static constexpr std::pair<SDL_GamepadButton, ImGuiKey> kButtons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
      {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
      {SDL_GAMEPAD_BUTTON_WEST, ImGuiKey_GamepadFaceLeft},
      {SDL_GAMEPAD_BUTTON_NORTH, ImGuiKey_GamepadFaceUp},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ImGuiKey_GamepadL1},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
      {SDL_GAMEPAD_BUTTON_START, ImGuiKey_GamepadStart},
  };
  bool any = false;
  bool down[std::size(kButtons)] = {};
  for (size_t i = 0; i < std::size(kButtons); ++i) {
    for (SDL_Gamepad* pad : pads_) down[i] = down[i] || SDL_GetGamepadButton(pad, kButtons[i].first);
    any = any || down[i];
  }
  if (!armed_) armed_ = !any;
  for (size_t i = 0; i < std::size(kButtons); ++i)
    io.AddKeyEvent(kButtons[i].second, armed_ && down[i]);
  float x = 0, y = 0;
  for (SDL_Gamepad* pad : pads_) {
    const float px = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    const float py = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    if (std::abs(px) > std::abs(x)) x = px;
    if (std::abs(py) > std::abs(y)) y = py;
  }
  auto stick = [&io](ImGuiKey key, float v) {
    constexpr float kDead = 0.35f;
    io.AddKeyAnalogEvent(key, v > kDead, v > kDead ? std::min(1.0f, v) : 0.0f);
  };
  stick(ImGuiKey_GamepadLStickLeft, -x);
  stick(ImGuiKey_GamepadLStickRight, x);
  stick(ImGuiKey_GamepadLStickUp, -y);
  stick(ImGuiKey_GamepadLStickDown, y);
}

}  // namespace tb2
