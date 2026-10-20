// thebigs2 - user settings file, display mode, and bring-your-own disc image.

#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <cctype>
#include <sstream>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef TB2_XEX_MD5
#define TB2_XEX_MD5 ""
#endif

REXCVAR_DEFINE_STRING(iso_path, "", "The Bigs 2",
                      "Disc image (.iso) to extract the game from on first launch; "
                      "preselected on the setup screen");
REXCVAR_DEFINE_STRING(display_resolution, "desktop", "The Bigs 2",
                      "Output resolution: 'desktop' or WIDTHxHEIGHT (e.g. 1920x1080). Window "
                      "size when windowed, display mode in exclusive fullscreen");
REXCVAR_DEFINE_DOUBLE(display_refresh_rate, 0.0, "The Bigs 2",
                      "Monitor refresh rate in Hz for exclusive fullscreen; 0 = desktop rate");
REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "The Bigs 2",
                      "Graphics API: 'vulkan' or 'd3d12' (Windows only, less tested)");

namespace tb2 {
namespace fs = std::filesystem;

namespace {

constexpr const char* kConfigName = "thebigs2.toml";

constexpr const char* kTemplate = R"TOML(# The Bigs 2 - settings
#
# Edit while the game is closed. Any setting can also be given on the command
# line for one run, e.g.  thebigs2 --resolution_scale=1
# Delete this file to get the defaults (and these comments) back.

# --- Game disc ---------------------------------------------------------------
# Your disc image: a full disc dump (.iso) or an XISO. On first launch its
# files are extracted to the game folder next to the AppImage / thebigs2.exe
# (or to ~/.local/share/thebigs2/game); after that the .iso isn't needed. The setup
# screen on first launch preselects this; the image you pick there is saved here.
iso_path = ""

# --- Display -----------------------------------------------------------------
# true = fullscreen, false = in a window.
fullscreen = true

# Fullscreen kind when fullscreen = true:
#   false = borderless: uses the desktop's own resolution and refresh rate
#           (display_resolution / display_refresh_rate are ignored).
#   true  = exclusive: switches the monitor to the mode below.
fullscreen_exclusive = false

# "desktop", or WIDTHxHEIGHT such as "1920x1080", "2560x1440", "3840x2160",
# "1280x800" (Steam Deck). In a window this is the window size; in exclusive
# fullscreen it is the monitor mode (the closest one it supports).
display_resolution = "desktop"

# Monitor refresh rate in Hz, exclusive fullscreen only. 0 = the desktop rate.
# The game runs at 60 fps in menus and 30 in matches (see frame_rate): 60 or 120
# give the smoothest motion; 144/165 will judder slightly unless you have VRR
# (G-Sync/FreeSync).
display_refresh_rate = 0

# Monitor to open on: 0 = system default, 1 = first, 2 = second, ...
monitor = 0

# Keep the game's 16:9 picture with black bars on other aspect ratios.
present_letterbox = true

@GPU_BACKEND@# --- Scaling -----------------------------------------------------------------
# Internal render scale: 1 = 1x (the game's own 1280x720), 2 = 2x (2560x1440),
# 3 = 3x (3840x2160). Higher is sharper and slower. Use 1 on GPUs without
# fragment shader interlock (AMD RADV, Steam Deck) - see
# render_target_path_vulkan - and with frame_rate = 60.
resolution_scale = 2

# How the picture is scaled to the screen: "bilinear" (default), "cas"
# (sharpened), "fsr" (AMD FSR 1 upscale).
present_effect = "bilinear"

# EDRAM emulation path: "fsi" (accurate; needed for resolution_scale > 1 in
# this game) or "host" (host render targets; faster, but at scale > 1 the
# bat, cap and shoes get green stripes). GPUs without fragment shader
# interlock always use "host".
render_target_path_vulkan = "fsi"

# Texture filtering: -1 = the game's own (as on the Xbox 360), 0 = off,
# 1-5 = force 1x/2x/4x/8x/16x anisotropic. Forcing it sharpens the field and
# stands at a distance and costs some GPU time.
anisotropic_override = 3

# The game's 4x MSAA on the main scene (smoother edges). On the Xbox 360 it
# needed the frame drawn in 3 tiles; without it the GPU emulation does about
# half the CPU work and ~20% less GPU work. Not set = automatic: on at 1x,
# off at 2x and up (the resolution scale already smooths edges) and on the
# Steam Deck; with native_renderer on at every scale except on the Deck.
# scene_msaa = true

# --- Frame rate --------------------------------------------------------------
# Frame rate in matches: 30 or 60. Menus always run at 60.
#   30 = the game's own cap, as on the Xbox 360 (default).
#   60 = EXPERIMENTAL. Game speed stays correct (the game steps by real time),
#        but it needs a fast CPU and GPU:
#        - desktop: holds 60 at resolution_scale = 1; at 2 the GPU may fall
#          short (about 53 fps on an RTX 4060), which looks worse than 30;
#        - Steam Deck: holds 60 in gameplay, slows down during transitions.
#        If it stutters, go back to 30.
frame_rate = 30

# --- Controls ----------------------------------------------------------------
# Keyboard key that quits (controller: hold Back + Start for 2 seconds).
bind_quit = "F10"

# --- Keyboard as controller --------------------------------------------------
# The keyboard plays as player 1's controller, together with the first pad.
# false = keyboard does nothing in game (the hotkeys above still work).
mnk_mode = true

# Each button takes one key or several, separated by commas ("Q,I"), and a key
# may need modifiers ("Shift+Up"). Modifiers must match exactly, so "Up" and
# "Shift+Up" can drive different buttons. Key names: A-Z, 0-9, F1-F24, Space,
# Return, Tab, Backspace, Escape, Delete, Insert, Home, End, PageUp, PageDown,
# Up, Down, Left, Right, Semicolon, Quote, Comma, Period, Slash, Backslash,
# Minus, Plus, LBracket, RBracket, Backtick, Numpad0-Numpad9, NumpadPlus,
# NumpadMinus, NumpadStar, NumpadSlash, NumpadEnter, CapsLock, LMB, RMB, MMB.
# Don't reuse the hotkeys F3, F4, F7, F8, F9, F10 or Backtick.
keybind_a = "Semicolon,Space"
keybind_b = "Quote,Backspace"
keybind_x = "L"
keybind_y = "P"
keybind_left_shoulder = "1"
keybind_right_shoulder = "3"
keybind_left_trigger = "Q,I"
keybind_right_trigger = "E,O"
keybind_back = "Z,Tab"
keybind_start = "X,Return"
keybind_lstick_up = "W"
keybind_lstick_down = "S"
keybind_lstick_left = "A"
keybind_lstick_right = "D"
keybind_lstick_press = "F"
keybind_rstick_up = "Up"
keybind_rstick_down = "Down"
keybind_rstick_left = "Left"
keybind_rstick_right = "Right"
keybind_rstick_press = "K"
keybind_dpad_up = "Shift+Up"
keybind_dpad_down = "Shift+Down"
keybind_dpad_left = "Shift+Left"
keybind_dpad_right = "Shift+Right"
)TOML";

fs::path PlatformConfigDir() {
#if defined(_WIN32)
  if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
    return fs::path(appdata) / "thebigs2";
  }
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    return fs::path(xdg) / "thebigs2";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return fs::path(home) / ".config" / "thebigs2";
  }
#endif
  return rex::filesystem::GetUserFolder() / "thebigs2";
}

bool InAppImage() {
  const char* appimage = std::getenv("APPIMAGE");
  return appimage && *appimage;
}

std::string LowerHex(const uint8_t* d, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kHex[d[i] >> 4];
    s += kHex[d[i] & 15];
  }
  return s;
}

// Opens a disc image and returns its root default.xex entry (or null).
struct OpenedDisc {
  std::unique_ptr<rex::filesystem::DiscImageDevice> device;
  rex::filesystem::DiscImageEntry* xex = nullptr;
};

OpenedDisc OpenDisc(const fs::path& iso) {
  OpenedDisc out;
  out.device = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\Cdrom0", iso);
  if (!out.device->Initialize()) {
    out.device.reset();
    return out;
  }
  out.xex =
      dynamic_cast<rex::filesystem::DiscImageEntry*>(out.device->ResolvePath("default.xex"));
  return out;
}

}  // namespace

fs::path ResolveConfigPath(const fs::path& exe_dir_default) {
  std::error_code ec;
  if (PortableMode()) return LauncherFolder() / "config" / kConfigName;
  if (!InAppImage() && fs::is_regular_file(exe_dir_default, ec)) {
    return exe_dir_default;  // portable: a config next to the binary wins
  }
  return PlatformConfigDir() / kConfigName;
}

bool SteamDeckProfile() {
  static const bool deck = [] {
    const char* profile = std::getenv("TB2_PROFILE");
    return profile && std::strcmp(profile, "steamdeck") == 0;
  }();
  return deck;
}

namespace {

// Appended to settings files written by older versions that lack it.
constexpr const char* kFrameRateSection = R"TOML(
# --- Frame rate --------------------------------------------------------------
# Frame rate in matches: 30 or 60. Menus always run at 60.
#   30 = the game's own cap, as on the Xbox 360 (default).
#   60 = EXPERIMENTAL. Game speed stays correct (the game steps by real time),
#        but it needs a fast CPU and GPU:
#        - desktop: holds 60 at resolution_scale = 1; at 2 the GPU may fall
#          short (about 53 fps on an RTX 4060), which looks worse than 30;
#        - Steam Deck: holds 60 in gameplay, slows down during transitions.
#        If it stutters, go back to 30.
frame_rate = 30
)TOML";

constexpr const char* kSteamDeckHeader = R"TOML(# Steam Deck build. These defaults are tuned for the Deck: the game renders at
# its own 1280x720 (resolution_scale = 1), which fills the 1280x800 screen edge
# to edge with thin bars top and bottom; host render targets (the Deck's GPU
# can't run "fsi"); the game's own texture filtering; and no keyboard
# controls. Matches run at the game's 30 fps; frame_rate = 60 is experimental:
# it holds 60 in gameplay and slows down during transitions.
#
)TOML";

// The template with the Steam Deck values (see kSteamDeckHeader).
std::string ConfigTemplate() {
  std::string text = kTemplate;
  const size_t gpu = text.find("@GPU_BACKEND@");
#if defined(_WIN32)
  text.replace(gpu, 13,
               "# Graphics API: \"vulkan\" (default; the settings below are tuned for it)\n"
               "# or \"d3d12\" (less tested; uses render_target_path_d3d12 instead).\n"
               "gpu_backend = \"vulkan\"\n\n");
#else
  text.erase(gpu, 13);
#endif
  if (!SteamDeckProfile()) return text;
  auto set = [&text](std::string_view from, std::string_view to) {
    const size_t pos = text.find(std::string("\n") + std::string(from) + "\n");
    if (pos != std::string::npos) text.replace(pos + 1, from.size(), to);
  };
  set("resolution_scale = 2", "resolution_scale = 1");
  set("render_target_path_vulkan = \"fsi\"", "render_target_path_vulkan = \"host\"");
  set("anisotropic_override = 3", "anisotropic_override = -1");
  set("mnk_mode = true", "mnk_mode = false");
  return kSteamDeckHeader + text;
}

}  // namespace

namespace {

// True if a top-level `key = ...` line exists.
bool HasSetting(const std::string& text, std::string_view key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line.compare(i, key.size(), key) != 0) continue;
    size_t j = line.find_first_not_of(" \t", i + key.size());
    if (j != std::string::npos && line[j] == '=') return true;
  }
  return false;
}

// Settings files are written once, on first launch: add sections that newer
// versions introduced, leaving everything else as the user has it.
void UpgradeConfigFile(const fs::path& config_path) {
  std::string text;
  {
    std::ifstream in(config_path, std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  if (HasSetting(text, "frame_rate")) return;
  // frame_rate replaces fps_60: carry an existing `fps_60 = true` over.
  std::string section = kFrameRateSection;
  std::string fps_60_line;
  {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      size_t i = line.find_first_not_of(" \t");
      if (i != std::string::npos && line.compare(i, 6, "fps_60") == 0) fps_60_line = line;
    }
  }
  if (fps_60_line.find("true") != std::string::npos) {
    section.replace(section.find("frame_rate = 30"), 15, "frame_rate = 60");
  }
  std::ofstream out(config_path, std::ios::binary | std::ios::app);
  if (!out) return;
  if (!text.empty() && text.back() != '\n') out << '\n';
  out << "\n# Added by a newer version of the game (frame_rate replaces fps_60).\n" << section;
}

}  // namespace

void EnsureConfigFile(const fs::path& config_path) {
  std::error_code ec;
  if (fs::exists(config_path, ec)) {
    UpgradeConfigFile(config_path);
    return;
  }
  fs::create_directories(config_path.parent_path(), ec);
  std::ofstream f(config_path, std::ios::binary);
  if (f) f << ConfigTemplate();
}

void SaveConfigString(const fs::path& config_path, std::string_view key, std::string_view value) {
  std::string text;
  {
    std::ifstream in(config_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  // TOML basic string: escape backslashes and quotes (Windows paths).
  std::string escaped;
  for (char c : value) {
    if (c == '\\' || c == '"') escaped += '\\';
    escaped += c;
  }
  const std::string line = std::string(key) + " = \"" + escaped + "\"";
  // Replace the first top-level `key = ...` line, else insert at the top.
  bool replaced = false;
  std::string result;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string_view cur(text.data() + pos, end - pos);
    size_t i = cur.find_first_not_of(" \t");
    std::string_view rest = i == std::string_view::npos ? std::string_view{} : cur.substr(i);
    if (!replaced && rest.starts_with(key)) {
      std::string_view after = rest.substr(key.size());
      size_t j = after.find_first_not_of(" \t");
      if (j != std::string_view::npos && after[j] == '=') {
        result += line;
        replaced = true;
        pos = end;
        if (pos < text.size()) result += '\n', ++pos;
        continue;
      }
    }
    result.append(cur);
    pos = end;
    if (pos < text.size()) result += '\n', ++pos;
  }
  text = replaced ? result : line + "\n" + text;
  std::error_code ec;
  fs::create_directories(config_path.parent_path(), ec);
  std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
  out << text;
}

#if defined(_WIN32)
// access() ignores ACLs on Windows (e.g. Program Files): try creating a file.
bool DirWritable(const fs::path& dir) {
  const fs::path probe = dir / ".thebigs2_write_test";
  { std::ofstream f(probe, std::ios::binary | std::ios::trunc); if (!f) return false; }
  std::error_code ec;
  fs::remove(probe, ec);
  return true;
}
#endif

bool ExeDirWritable() {
  if (InAppImage()) return false;
#if defined(_WIN32)
  static const bool writable = DirWritable(rex::filesystem::GetExecutableFolder());
  return writable;
#else
  return access(rex::filesystem::GetExecutableFolder().c_str(), W_OK) == 0;
#endif
}

fs::path LauncherFolder() {
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    return fs::path(appimage).parent_path();
  }
  return rex::filesystem::GetExecutableFolder();
}

bool PortableMode() {
  static const bool portable = [] {
#if defined(_WIN32)
    // The Windows zip is a folder like the AppImage's: everything next to the
    // exe, unless it was put somewhere read-only (Program Files).
    return ExeDirWritable();
#else
    if (!InAppImage()) return false;
    if (access(LauncherFolder().c_str(), W_OK) == 0) return true;
    // Logging isn't up yet; stderr is all there is.
    std::fprintf(stderr,
                 "thebigs2: %s is not writable, using ~/.config and ~/.local/share instead "
                 "of a portable setup\n",
                 LauncherFolder().c_str());
    return false;
#endif
  }();
  return portable;
}

void MigrateToPortable(const fs::path& old_user_data) {
  std::error_code ec;
  const fs::path old_config = PlatformConfigDir() / kConfigName;
  const fs::path root = LauncherFolder();
  const fs::path saves = root / "saves";
  if (!fs::exists(saves, ec) && fs::is_directory(old_user_data, ec)) {
    // Profiles (save games) and achievements; not the extracted game, the
    // shader cache or logs, which are rebuilt or not worth carrying over.
    fs::create_directories(saves, ec);
    for (const auto& e : fs::directory_iterator(old_user_data, ec)) {
      const std::string name = e.path().filename().string();
      if (!e.is_directory(ec) || name == "game" || name == "game.partial" || name == "cache" ||
          name == "logs" || name == "traces") {
        continue;
      }
      fs::copy(e.path(), saves / name, fs::copy_options::recursive, ec);
      std::fprintf(stderr, "thebigs2: copied %s to %s%s\n", e.path().string().c_str(),
                   (saves / name).string().c_str(), ec ? (" FAILED: " + ec.message()).c_str() : "");
    }
  }
  const fs::path config = root / "config" / kConfigName;
  // Not into a Steam Deck folder: a desktop install's settings (2x, fsi) would
  // replace the Deck defaults. Saves are fine to carry over.
  if (!SteamDeckProfile() && !fs::exists(config, ec) && fs::is_regular_file(old_config, ec)) {
    fs::create_directories(config.parent_path(), ec);
    fs::copy_file(old_config, config, ec);
    std::fprintf(stderr, "thebigs2: copied %s to %s%s\n", old_config.string().c_str(), config.string().c_str(),
                 ec ? (" FAILED: " + ec.message()).c_str() : "");
  }
}

std::string IsoPathSetting() { return REXCVAR_GET(iso_path); }

void SetIsoPathSetting(const std::string& path) {
  rex::cvar::SetFlagByName("iso_path", path);
}

namespace {

constexpr const char* kMarker = ".extracted";

bool XexMatches(const rex::filesystem::DiscImageEntry* xex, std::string* actual_out) {
  const std::string expected = TB2_XEX_MD5;
  if (expected.empty()) return true;
  uint8_t digest[16];
  TB2_Md5(xex->mmap()->data() + xex->data_offset(), xex->size(), digest);
  std::string actual = LowerHex(digest, 16);
  if (actual_out) *actual_out = actual;
  return actual == expected;
}

using FileList = std::vector<std::pair<fs::path, const rex::filesystem::DiscImageEntry*>>;

void CollectFiles(const rex::filesystem::Entry* dir, const fs::path& rel, FileList& files,
                  std::vector<fs::path>& dirs) {
  for (const auto& child : dir->children()) {
    fs::path p = rel / child->name();
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      dirs.push_back(p);
      CollectFiles(child.get(), p, files, dirs);
    } else if (auto* f = dynamic_cast<const rex::filesystem::DiscImageEntry*>(child.get())) {
      files.emplace_back(p, f);
    }
  }
}

}  // namespace

bool CheckIso(const fs::path& iso, std::string& error) {
  std::error_code ec;
  if (!fs::is_regular_file(iso, ec)) {
    error = "Disc image not found:\n" + iso.string();
    return false;
  }
  OpenedDisc disc = OpenDisc(iso);
  if (!disc.device) {
    error = "This file is not an Xbox 360 disc image (or it is damaged):\n" + iso.string();
    return false;
  }
  if (!disc.xex) {
    error = "This disc image has no default.xex - it is not a game disc:\n" + iso.string();
    return false;
  }
  std::string actual;
  if (!XexMatches(disc.xex, &actual)) {
    error = "This disc image is not the version of The Bigs 2 this build was made from.\n\n" +
            iso.string() + "\n\ndefault.xex MD5: " + actual + "\nexpected:        " +
            TB2_XEX_MD5;
    return false;
  }
  return true;
}

fs::path FindIsoNearLauncher() {
  std::error_code ec;
  std::vector<fs::path> dirs = {LauncherFolder(), LauncherFolder() / "game"};
  for (const auto& dir : dirs) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      auto ext = e.path().extension().string();
      for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (ext != ".iso" || !e.is_regular_file(ec)) continue;
      OpenedDisc disc = OpenDisc(e.path());
      if (disc.xex && XexMatches(disc.xex, nullptr)) return e.path();
    }
  }
  return {};
}

bool IsExtractedGame(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_regular_file(dir / "default.xex", ec)) return false;
  std::ifstream in(dir / kMarker);
  if (!in) return false;
  std::string md5;
  in >> md5;
  // An extraction made for a different build (other XEX) doesn't count.
  const std::string expected = TB2_XEX_MD5;
  return expected.empty() || md5 == expected;
}

bool ExtractIso(const fs::path& iso, const fs::path& dest,
                const std::function<bool(int)>& progress, std::string& error) {
  OpenedDisc disc = OpenDisc(iso);
  if (!disc.device || !disc.xex) {
    error = "Could not read the disc image:\n" + iso.string();
    return false;
  }
  FileList files;
  std::vector<fs::path> dirs;
  CollectFiles(disc.device->root(), {}, files, dirs);
  uint64_t total = 0;
  for (auto& [p, f] : files) total += f->size();

  std::error_code ec;
  const fs::path partial = dest.string() + ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec) {
    error = "Could not create " + partial.string() + ":\n" + ec.message();
    return false;
  }
  const auto space = fs::space(partial, ec);
  if (!ec && space.available < total + (64ull << 20)) {
    error = "Not enough free space to extract the game.\n\nNeeded: " +
            std::to_string((total >> 20) + 64) + " MB\nFree:   " +
            std::to_string(space.available >> 20) + " MB\nin " + partial.string();
    fs::remove_all(partial, ec);
    return false;
  }
  for (const auto& d : dirs) fs::create_directories(partial / d, ec);

  REXLOG_INFO("Extracting {} ({} files, {} MB) to {}", iso.string(), files.size(), total >> 20,
              dest.string());
  uint64_t done = 0;
  int last_pct = -1;
  constexpr size_t kChunk = 8u << 20;
  for (const auto& [rel, f] : files) {
    std::ofstream out(partial / rel, std::ios::binary | std::ios::trunc);
    const char* src = reinterpret_cast<const char*>(f->mmap()->data() + f->data_offset());
    size_t left = f->size();
    while (out && left) {
      size_t n = std::min(left, kChunk);
      out.write(src, std::streamsize(n));
      src += n;
      left -= n;
      done += n;
      int pct = total ? int(done * 100 / total) : 100;
      if (pct != last_pct) {
        last_pct = pct;
        if (!progress(pct)) {
          out.close();
          fs::remove_all(partial, ec);
          error = "cancelled";
          return false;
        }
      }
    }
    out.close();
    if (!out) {
      error = "Failed writing " + (partial / rel).string() + "\n(disk full or no permission?)";
      fs::remove_all(partial, ec);
      return false;
    }
  }
  {
    std::ofstream marker(partial / kMarker);
    marker << TB2_XEX_MD5 << "\n" << iso.string() << "\n";
  }
  fs::remove_all(dest, ec);
  fs::rename(partial, dest, ec);
  if (ec) {
    error = "Could not move the extracted game into place:\n" + ec.message();
    return false;
  }
  REXLOG_INFO("Extraction finished");
  return true;
}

// --- Display ----------------------------------------------------------------

namespace {

SDL_Window* GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* w = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return w;
}

bool ParseResolution(int& w, int& h) {
  const std::string value = REXCVAR_GET(display_resolution);
  if (value.empty() || value == "desktop" || value == "native") return false;
  return std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

void ApplyExclusiveMode(SDL_Window* win) {
  if (!rex::cvar::Query<bool>("fullscreen_exclusive")) return;

  const SDL_DisplayID display = SDL_GetDisplayForWindow(win);
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
  int w = desktop ? desktop->w : 0, h = desktop ? desktop->h : 0;
  ParseResolution(w, h);
  const float hz = float(REXCVAR_GET(display_refresh_rate));
  if (w <= 0 || h <= 0) return;

  SDL_DisplayMode mode{};
  if (!SDL_GetClosestFullscreenDisplayMode(display, w, h, hz > 0 ? hz : 0.0f, true, &mode)) {
    REXLOG_WARN("Display has no mode near {}x{} @ {} Hz: {}", w, h, hz, SDL_GetError());
    return;
  }
  const SDL_DisplayMode* cur = SDL_GetWindowFullscreenMode(win);
  if (cur && cur->w == mode.w && cur->h == mode.h && cur->refresh_rate == mode.refresh_rate &&
      cur->pixel_density == mode.pixel_density) {
    REXLOG_INFO("Display mode {}x{} @ {:.3g} Hz", mode.w, mode.h, mode.refresh_rate);
    return;
  }
  if (!SDL_SetWindowFullscreenMode(win, &mode)) {
    REXLOG_WARN("Could not switch to {}x{} @ {:.3g} Hz: {}", mode.w, mode.h, mode.refresh_rate,
                SDL_GetError());
    return;
  }
  REXLOG_INFO("Display mode {}x{} @ {:.3g} Hz", mode.w, mode.h, mode.refresh_rate);
}

std::function<void(std::function<void()>)> g_post_to_ui;

bool DisplayEventWatch(void*, SDL_Event* e) {
  if (e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
      e->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
    if (g_post_to_ui) g_post_to_ui([] { ApplyDisplaySettings(false); });
  }
  return true;
}

}  // namespace

void ApplyDisplaySettings(bool startup) {
  SDL_Window* win = GameWindow();
  if (!win) return;
  // SDL keeps the exclusive mode with the window and uses it whenever the
  // window is fullscreen, so this is safe while a fullscreen switch is pending.
  if (rex::cvar::Query<bool>("fullscreen") ||
      (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
    ApplyExclusiveMode(win);
  } else if (startup) {
    int w = 0, h = 0;
    if (ParseResolution(w, h)) {
      SDL_SetWindowSize(win, w, h);
      REXLOG_INFO("Window size {}x{}", w, h);
      SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
  }
}

void InstallDisplayWatch(std::function<void(std::function<void()>)> post_to_ui) {
  g_post_to_ui = std::move(post_to_ui);
  SDL_AddEventWatch(DisplayEventWatch, nullptr);
}

}  // namespace tb2
