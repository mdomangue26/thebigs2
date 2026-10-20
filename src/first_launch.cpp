// thebigs2 - first-launch setup screen. Drawn with the SDK's Dear ImGui in the
// game window (so it works fullscreen and in Steam Deck Game Mode, where
// system file pickers may not show up): a small file browser for the disc
// image, an immediate check that it is the right disc, then extraction with a
// progress bar. Mouse, touch, keyboard and controller (A select, B up a
// folder) all work.

#include "first_launch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "imgui_gamepad.h"
#include "settings.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tb2 {
namespace fs = std::filesystem;

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
// Roboto Medium (Apache 2.0, assets/fonts/LICENSE-Roboto.txt).
const unsigned char kRoboto[] = {
#embed "../assets/fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// The SDK's ImGui renderer has a fixed font atlas, so bake a few sizes and
// pick the one that suits the window height (800 on the Deck, 1440 here).
constexpr float kFontSizes[] = {18, 24, 32, 44, 60, 80, 110};
constexpr int kFontCount = int(std::size(kFontSizes));
ImFont* g_fonts[kFontCount] = {};

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string FormatSize(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ull << 30)) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / double(1ull << 20));
  }
  return buf;
}

// CheckIso's errors end with MD5 details for the log; keep the screen short.
std::string ShortError(std::string error) {
  if (size_t cut = error.find("\n\ndefault.xex MD5"); cut != std::string::npos) error.resize(cut);
  return error;
}

bool IsDiscImage(const fs::path& p) {
  const std::string ext = Lower(p.extension().string());
  return ext == ".iso" || ext == ".xiso";
}

struct Entry {
  std::string name;
  fs::path path;
  bool dir = false;
  uint64_t size = 0;
};

// Folders worth a shortcut: the game's folder, home, Downloads, and mounted
// SD cards / USB drives (/run/media/<user>/<label> on SteamOS).
std::vector<std::pair<std::string, fs::path>> Places(const fs::path& start_dir) {
  std::vector<std::pair<std::string, fs::path>> out;
  std::error_code ec;
  out.emplace_back("Game folder", start_dir);
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home && *home) {
    out.emplace_back("Home", fs::path(home));
    if (fs::is_directory(fs::path(home) / "Downloads", ec)) {
      out.emplace_back("Downloads", fs::path(home) / "Downloads");
    }
  }
#if defined(_WIN32)
  const DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (drives & (1u << i)) {
      const std::string root = std::string(1, char('A' + i)) + ":\\";
      out.emplace_back("Drive " + root, fs::path(root));
    }
  }
#else
  const char* user = std::getenv("USER");
  for (const char* base : {"/run/media", "/media"}) {
    for (const auto& e : fs::directory_iterator(base, ec)) {
      if (!e.is_directory(ec)) continue;
      const std::string name = e.path().filename().string();
      if ((user && name == user) || name == "deck") {
        for (const auto& d : fs::directory_iterator(e.path(), ec)) {
          if (d.is_directory(ec)) out.emplace_back("Drive: " + d.path().filename().string(), d.path());
        }
      } else {
        out.emplace_back("Drive: " + name, e.path());
      }
    }
  }
#endif
  return out;
}

class FirstLaunchDialog;
FirstLaunchDialog* g_dialog = nullptr;

class FirstLaunchDialog final : public rex::ui::ImGuiDialog {
 public:
  FirstLaunchDialog(rex::ui::ImGuiDrawer* drawer, FirstLaunchSetup setup, ExtractStatus* status)
      : ImGuiDialog(drawer), setup_(std::move(setup)), status_(status) {
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    saved_backend_flags_ = io.BackendFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    message_ = ShortError(setup_.message);
    places_ = Places(setup_.start_dir);
    std::error_code ec;
    if (!setup_.suggested_iso.empty()) {
      Open(setup_.suggested_iso.parent_path());
      Select(setup_.suggested_iso);
    } else {
      Open(fs::is_directory(setup_.start_dir, ec) ? setup_.start_dir : fs::current_path(ec));
    }
  }

  ~FirstLaunchDialog() override {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = saved_config_flags_;
    io.BackendFlags = saved_backend_flags_;
    if (g_dialog == this) g_dialog = nullptr;
  }

  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    gamepads_.Feed(io);

    const bool running = status_->running.load();
    if (was_running_ && !running) {
      std::lock_guard lock(status_->mutex);
      message_ = status_->error;  // empty on success (the screen closes then)
    }
    was_running_ = running;

    // Font sizes: body text ~1/30 of the window height, the title 3 steps up.
    int body = 0;
    while (body + 1 < kFontCount && kFontSizes[body + 1] <= io.DisplaySize.y / 30.0f) ++body;
    ImFont* body_font = g_fonts[body];
    ImFont* title_font = g_fonts[std::min(body + 3, kFontCount - 1)];
    if (!body_font) return;  // fonts weren't added (OnConfigureFonts not called)
    const float u = body_font->LegacySize;

    ImGui::PushFont(body_font, body_font->LegacySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(u * 1.6f, u * 1.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(u * 0.7f, u * 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(u * 0.5f, u * 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, u * 0.8f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, u * 0.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, u * 0.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.36f, 0.70f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.45f, 0.82f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.30f, 0.60f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.20f, 0.60f, 0.35f, 1.0f));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (first_frame_) {
      // Keyboard/controller navigation needs a focused window from the start.
      ImGui::SetNextWindowFocus();
      first_frame_ = false;
    }
    ImGui::Begin("##tb2_setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushFont(title_font, title_font->LegacySize);
    ImGui::TextUnformatted("The Bigs 2");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "First-time setup");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Choose your The Bigs 2 disc image (.iso). Its files are copied once to "
        "the folder below; after that the .iso isn't needed.");
    ImGui::TextColored(kMuted, "%s", setup_.dest.string().c_str());
    if (!message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kBad);
      ImGui::TextWrapped("%s", message_.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (running) {
      DrawExtracting(u);
    } else {
      DrawBrowser(u);
    }

    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(7);
    ImGui::PopFont();
  }

 private:
  static constexpr ImVec4 kMuted{0.62f, 0.68f, 0.78f, 1.0f};
  static constexpr ImVec4 kGood{0.45f, 0.85f, 0.50f, 1.0f};
  static constexpr ImVec4 kBad{1.0f, 0.45f, 0.40f, 1.0f};

  void DrawExtracting(float u) {
    const int pct = status_->percent.load();
    ImGui::Spacing();
    ImGui::Text("Extracting %s", selected_.filename().string().c_str());
    char label[16];
    std::snprintf(label, sizeof(label), "%d%%", pct);
    ImGui::ProgressBar(float(pct) / 100.0f, ImVec2(-1, u * 1.8f), label);
    ImGui::TextColored(kMuted, "This takes a few minutes. The game starts when it's done.");
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(u * 7, 0))) setup_.cancel();
  }

  void DrawBrowser(float u) {
    // B / Backspace: up one folder.
    if (!ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
      Open(dir_.parent_path());
    }

    const float footer = u * 4.6f;
    const float list_h = std::max(u * 6, ImGui::GetContentRegionAvail().y - footer);

    ImGui::BeginChild("##places", ImVec2(ImGui::GetContentRegionAvail().x * 0.26f, list_h),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    for (const auto& [label, path] : places_) {
      if (ImGui::Selectable(label.c_str(), dir_ == path)) Open(path);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##files", ImVec2(0, list_h),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    ImGui::TextColored(kMuted, "%s", dir_.string().c_str());
    if (dir_.has_parent_path() && dir_.parent_path() != dir_ &&
        ImGui::Selectable(".. (up one folder)")) {
      Open(dir_.parent_path());
    }
    if (!list_error_.empty()) ImGui::TextColored(kBad, "%s", list_error_.c_str());
    if (entries_.empty() && list_error_.empty()) {
      ImGui::TextColored(kMuted, "No folders or disc images here.");
    }
    fs::path open_next;
    for (const auto& e : entries_) {
      ImGui::PushID(e.name.c_str());
      if (e.dir) {
        if (ImGui::Selectable((e.name + "/").c_str())) open_next = e.path;
      } else {
        if (ImGui::Selectable(e.name.c_str(), selected_ == e.path)) Select(e.path);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - u * 4);
        ImGui::TextColored(kMuted, "%s", FormatSize(e.size).c_str());
      }
      if (focus_selected_ && e.path == selected_) {
        ImGui::SetItemDefaultFocus();
        focus_selected_ = false;
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (!open_next.empty()) Open(open_next);

    // Footer: the chosen image and the buttons.
    if (selected_.empty()) {
      ImGui::TextColored(kMuted, "Select your disc image in the list.");
    } else if (selected_ok_) {
      ImGui::TextColored(kGood, "%s - the right disc, ready to extract.",
                         selected_.filename().string().c_str());
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kBad);
      ImGui::TextWrapped("%s", selected_error_.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!selected_ok_);
    if (ImGui::Button("Extract and play", ImVec2(u * 10, 0))) {
      message_.clear();
      setup_.extract(selected_);
    }
    if (selected_ok_ && focus_extract_) {
      ImGui::SetItemDefaultFocus();
      ImGui::SetKeyboardFocusHere(-1);
      ImGui::SetNavCursorVisible(true);  // so the first Enter / A presses it
      focus_extract_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(u * 5, 0))) setup_.quit();
  }

  void Open(const fs::path& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
      list_error_ = "Can't open " + dir.string() + ": " + ec.message();
      return;
    }
    list_error_.clear();
    dir_ = dir;
    entries_.clear();
    for (const auto& e : it) {
      const std::string name = e.path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      Entry entry{name, e.path(), e.is_directory(ec), 0};
      if (!entry.dir) {
        if (!e.is_regular_file(ec) || !IsDiscImage(e.path())) continue;
        entry.size = e.file_size(ec);
      }
      entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
      if (a.dir != b.dir) return !a.dir;  // disc images first, they're what you want
      return Lower(a.name) < Lower(b.name);
    });
  }

  void Select(const fs::path& iso) {
    selected_ = iso;
    selected_error_.clear();
    selected_ok_ = CheckIso(iso, selected_error_);
    selected_error_ = ShortError(selected_error_);
    focus_selected_ = true;
    focus_extract_ = selected_ok_;
    if (selected_ok_) message_.clear();
  }

  FirstLaunchSetup setup_;
  ExtractStatus* status_;
  ImGuiConfigFlags saved_config_flags_ = 0;
  ImGuiBackendFlags saved_backend_flags_ = 0;
  ImGuiGamepads gamepads_;  // the SDK's input system isn't running yet

  std::vector<std::pair<std::string, fs::path>> places_;
  fs::path dir_;
  std::vector<Entry> entries_;
  std::string list_error_;

  fs::path selected_;
  bool selected_ok_ = false;
  std::string selected_error_;
  bool focus_selected_ = false;
  bool focus_extract_ = false;

  std::string message_;
  bool was_running_ = false;
  bool first_frame_ = true;
};

}  // namespace

void AddFirstLaunchFonts(ImFontAtlas* atlas) {
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
  for (int i = 0; i < kFontCount; ++i) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static data
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    g_fonts[i] = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRoboto),
                                             int(sizeof(kRoboto)), kFontSizes[i], &cfg, kRanges);
  }
}

void ShowFirstLaunch(rex::ui::ImGuiDrawer* drawer, FirstLaunchSetup setup, ExtractStatus* status) {
  if (g_dialog) return;
  g_dialog = new FirstLaunchDialog(drawer, std::move(setup), status);  // deletes itself on close
}

void CloseFirstLaunch() {
  if (g_dialog) g_dialog->RequestClose();
  g_dialog = nullptr;
}

}  // namespace tb2
