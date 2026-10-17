// thebigs2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/system.h>

#include <SDL3/SDL.h>

#include "first_launch.h"
#include "frame_stats.h"
#include "settings.h"
#include "native_capture.h"
#if defined(TB2_RENDER_TRACE)
#include "render_trace.h"
#endif

void TB2_RegisterRenderDocCaptureBind();  // renderdoc_capture.cpp

class Thebigs2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Thebigs2App>(new Thebigs2App(ctx, "thebigs2",
        PPCImageConfig));
  }

  // GPU emulation is opt-in; default to the Xenos plugin unless the user
  // picked one with --gpu_plugin.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

    // Present with plain FIFO (vsync) by default. The SDK prefers immediate,
    // then mailbox, which redraws the latest guest frame 1000+ times a second;
    // guest frames (60 Hz emulated vblank) then land unevenly and the game
    // hitches. Only flags still at their compiled-in default are changed, so
    // --vulkan_allow_present_mode_*=true on the command line or in a config
    // file still wins.
    for (const char* flag : {"vulkan_allow_present_mode_immediate",
                             "vulkan_allow_present_mode_mailbox",
                             "vulkan_allow_present_mode_fifo_relaxed"}) {
      if (rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(flag, "false");
      }
    }

    // Render at 2x (clean edges at 1440p) through the fragment-shader-interlock
    // EDRAM path. With the default host-render-target path, 2x scaling breaks
    // the game's 2xMSAA targets (bat/shoes/cap come out as green column
    // stripes). The SDK falls back to host render targets on GPUs without
    // fragmentShaderSampleInterlock (e.g. RADV/Steam Deck) — use
    // --resolution_scale=1 there.
    auto set_default = [](const char* flag, const char* value) {
      if (rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(flag, value);
      }
    };
    // These cvars live in the GPU plugin and only exist once it is loaded, so
    // load it here (ReXApp skips its own load when config.graphics is set).
    // Vulkan by default on Windows too, where the SDK would pick D3D12: the
    // settings below and several SDK fixes (resolve shader) are Vulkan-only.
    if (!config.graphics && !config.gpu_plugin.empty()) {
      config.graphics = rex::system::LoadGpuPlugin(
          config.gpu_plugin, rex::cvar::GetFlagByName("gpu_backend"));
    }
    if (tb2::SteamDeckProfile()) {
      // Steam Deck AppImage: RADV has no fragment shader interlock, so host
      // render targets at 1x (the game's 1280x720 fills the 1280x800 screen's
      // width 1:1). The game's own texture filtering instead of the forced 4x
      // anisotropic, and no keyboard: Steam Input can send keys from the
      // trackpads/back buttons, which would also drive player 1.
      set_default("render_target_path_vulkan", "host");
      set_default("resolution_scale", "1");
      set_default("anisotropic_override", "-1");
      set_default("mnk_mode", "false");
      REXLOG_INFO("Steam Deck profile (TB2_PROFILE=steamdeck)");
    } else {
      set_default("render_target_path_vulkan", "fsi");
      set_default("resolution_scale", "2");
      // The keyboard acts as player 1's controller (merged with the first
      // pad, so both work at once). Bindings are the keybind_* settings.
      set_default("mnk_mode", "true");
    }

    // Main scene 4x MSAA (render_tweaks.cpp). Without it the game also drops
    // its 3-tile rendering: about half the GPU command thread's work and ~20%
    // of the GPU's. At 2x and up the resolution scaling already smooths the
    // edges (the image is practically the same), and the Deck needs the
    // headroom, so it is only kept at 1x on the desktop.
    if (tb2::SteamDeckProfile() ||
        std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()) >= 2) {
      set_default("scene_msaa", "false");
    }
  }

  // Clean quit without Alt+F4. Both routes close the window, which runs the
  // SDK's normal shutdown (TerminateTitle + log flush):
  //   - keyboard: F10 (rebindable via the bind_quit setting)
  //   - controller: hold Back + Start for 2 seconds
  void OnPostSetup() override {
    rex::ui::RegisterBind("bind_quit", "F10", "Quit the game",
                          [this] { RequestQuit("keyboard"); });
    TB2_RegisterRenderDocCaptureBind();
    SetGuestFrameStats(&tb2::GuestFrameStats);
    tb2::NativeCaptureInit(tb2_user_data_root_ / "captures");
#if defined(TB2_RENDER_TRACE)
    TB2TraceInit(tb2_user_data_root_ / "traces");
#endif

    // Display mode from the settings file (resolution / refresh rate).
    app_context().CallInUIThreadDeferred([] { tb2::ApplyDisplaySettings(true); });
    tb2::InstallDisplayWatch([this](std::function<void()> fn) {
      app_context().CallInUIThreadDeferred(std::move(fn));
    });

    quit_poller_ = std::thread([this] {
      using namespace std::chrono;
      constexpr uint16_t kCombo =
          rex::input::X_INPUT_GAMEPAD_BACK | rex::input::X_INPUT_GAMEPAD_START;
      constexpr auto kHold = seconds(2);
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      steady_clock::time_point held_since{};
      bool holding = false;
      while (!stop_poller_.load(std::memory_order_relaxed)) {
        bool pressed = false;
        rex::input::X_INPUT_STATE state{};
        for (uint32_t user = 0; input && user < 4 && !pressed; ++user) {
          if (input->GetStateForUI(user, &state) == 0 &&
              (uint16_t(state.gamepad.buttons) & kCombo) == kCombo) {
            pressed = true;
          }
        }
        const auto now = steady_clock::now();
        if (pressed && !holding) {
          holding = true;
          held_since = now;
        } else if (!pressed) {
          holding = false;
        } else if (now - held_since >= kHold) {
          RequestQuit("controller (Back+Start)");
          return;
        }
        std::this_thread::sleep_for(milliseconds(50));
      }
    });
  }

  void OnShutdown() override {
    stop_poller_.store(true, std::memory_order_relaxed);
    if (quit_poller_.joinable()) quit_poller_.join();
  }

  // Settings live in thebigs2.toml: next to the executable if one is there
  // (portable), else ~/.config/thebigs2/ (%APPDATA%\thebigs2\ on Windows).
  // A commented template is written on first launch.
  //
  // Game data, first match wins: --game_data_root; a `game/` folder (with
  // default.xex) next to the executable/AppImage or in the working directory;
  // otherwise a disc image (see OnFinalizePaths).
  //
  // Portable AppImage (see tb2::PortableMode): saves and the shader cache go
  // to saves/ and cache/ next to the AppImage, unless set on the command line.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (tb2::PortableMode()) {
      const auto root = tb2::LauncherFolder();
      tb2::MigrateToPortable(paths.user_data_root);
      if (rex::cvar::GetFlagSource("user_data_root") == rex::cvar::Source::kDefault) {
        paths.user_data_root = root / "saves";
      }
      if (rex::cvar::GetFlagSource("cache_root") == rex::cvar::Source::kDefault) {
        paths.cache_root = root / "cache";
      }
    }
    paths.config_path = tb2::ResolveConfigPath(paths.config_path);
    tb2::EnsureConfigFile(paths.config_path);
    tb2_config_path_ = paths.config_path;
    tb2_user_data_root_ = paths.user_data_root;

    if (!paths.game_data_root.empty()) return;
    std::error_code ec;
    for (const auto& candidate :
         {rex::filesystem::GetExecutableFolder() / "game", tb2::LauncherFolder() / "game",
          std::filesystem::current_path(ec) / "game"}) {
      if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
  }

  // Logs go to logs/ next to a portable AppImage, next to the executable when
  // it is writable, else to the user data folder (~/.local/share/thebigs2/logs).
  void OnConfigureLogging(rex::LogConfig& config) override {
    if (tb2::PortableMode()) {
      config.log_dir = tb2::LauncherFolder() / "logs";
    } else if (!tb2::ExeDirWritable()) {
      config.log_dir = tb2_user_data_root_ / "logs";
    }
  }

  // The setup screen's fonts (Roboto at a few sizes; first_launch.cpp).
  void OnConfigureFonts(ImFontAtlas* atlas) override { tb2::AddFirstLaunchFonts(atlas); }

  // No game folder: extract the disc image once, into game/ next to a portable
  // AppImage or else the user data folder (~/.local/share/thebigs2/game), then
  // run from there. The setup screen (first_launch.cpp) picks the image,
  // preselecting iso_path from the settings file or a matching .iso next to
  // the executable/AppImage.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    if (!defaults.game_data_root.empty()) return defaults;
    if (tb2::IsExtractedGame(extracted_dir())) {
      rex::PathConfig paths = defaults;
      paths.game_data_root = extracted_dir();
      return paths;
    }

    pending_paths_ = defaults;
    resume_ = std::move(resume);

    tb2::FirstLaunchSetup setup;
    setup.dest = extracted_dir();
    setup.start_dir = tb2::LauncherFolder();
    if (const std::filesystem::path iso = tb2::IsoPathSetting(); !iso.empty()) {
      std::string error;
      if (tb2::CheckIso(iso, error)) {
        setup.suggested_iso = iso;
      } else {
        REXLOG_ERROR("{}", error);
        setup.message = "iso_path in the settings file: " + error;
      }
    }
    if (setup.suggested_iso.empty()) setup.suggested_iso = tb2::FindIsoNearLauncher();
    setup.extract = [this](const std::filesystem::path& iso) { StartExtract(iso); };
    setup.cancel = [this] { user_cancel_.store(true); };
    setup.quit = [this] {
      REXLOG_INFO("Quit from the setup screen");
      app_context().QuitFromUIThread();
    };

    if (!imgui_drawer()) {  // no presenter to draw the screen with
      if (setup.suggested_iso.empty()) {
        rex::ShowSimpleMessageBox(
            rex::SimpleMessageBoxType::Error,
            "No game files. Set iso_path in\n" + tb2_config_path_.string() +
                "\nor start with --iso_path=/path/to/TheBigs2.iso");
        return defaults;
      }
      StartExtract(setup.suggested_iso);
      return std::nullopt;
    }
    tb2::ShowFirstLaunch(imgui_drawer(), std::move(setup), &extract_status_);
    return std::nullopt;
  }

  ~Thebigs2App() override {
    shutting_down_.store(true);
    if (extract_thread_.joinable()) extract_thread_.join();
  }

 private:
  std::filesystem::path extracted_dir() const {
    return tb2::PortableMode() ? tb2::LauncherFolder() / "game" : tb2_user_data_root_ / "game";
  }

  // Extraction runs on a worker thread; the setup screen shows its progress.
  void StartExtract(const std::filesystem::path& iso) {
    if (extract_status_.running.load()) return;
    if (extract_thread_.joinable()) extract_thread_.join();
    tb2::SetIsoPathSetting(iso.string());
    tb2::SaveConfigString(tb2_config_path_, "iso_path", iso.string());
    user_cancel_.store(false);
    extract_status_.percent.store(0);
    extract_status_.running.store(true);

    extract_thread_ = std::thread([this, iso] {
      std::string error;
      bool ok = tb2::ExtractIso(iso, extracted_dir(), [this](int pct) {
        extract_status_.percent.store(pct);
        return !user_cancel_.load() && !shutting_down_.load();
      }, error);
      if (shutting_down_.load()) return;
      if (user_cancel_.load()) error = "Extraction cancelled.";
      app_context().CallInUIThreadDeferred([this, ok, error] { OnExtractDone(ok, error); });
    });
  }

  void OnExtractDone(bool ok, const std::string& error) {
    if (extract_thread_.joinable()) extract_thread_.join();
    {
      std::lock_guard lock(extract_status_.mutex);
      extract_status_.error = ok || user_cancel_.load() ? (ok ? "" : error)
                                                         : "Extracting the disc image failed: " + error;
    }
    if (!ok) {
      REXLOG_ERROR("Extraction failed: {}", error);
      extract_status_.running.store(false);
      if (!imgui_drawer()) app_context().QuitFromUIThread();
      return;
    }
    tb2::CloseFirstLaunch();
    extract_status_.running.store(false);
    rex::PathConfig paths = pending_paths_;
    paths.game_data_root = extracted_dir();
    auto resume = std::move(resume_);
    resume(std::move(paths));
  }

  std::filesystem::path tb2_config_path_;
  std::filesystem::path tb2_user_data_root_;
  rex::PathConfig pending_paths_;
  std::function<void(rex::PathConfig)> resume_;
  std::thread extract_thread_;
  tb2::ExtractStatus extract_status_;
  std::atomic<bool> user_cancel_{false};
  std::atomic<bool> shutting_down_{false};

  // Safe from any thread: the close itself always runs on the UI thread.
  void RequestQuit(const char* source) {
    if (quitting_.exchange(true)) return;
    REXLOG_INFO("Quit requested via {}", source);
    app_context().CallInUIThread([this] {
      if (window()) window()->RequestClose();
    });
  }

  std::thread quit_poller_;
  std::atomic<bool> stop_poller_{false};
  std::atomic<bool> quitting_{false};
};
