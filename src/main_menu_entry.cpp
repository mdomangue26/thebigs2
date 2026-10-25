// thebigs2 - our entries on the game's main menu (main_menu_entry.h).
//
// The main menu is the screen file title.txt in data/frontend/mainmenu.big:
// text ("cFEScreen TitleScreen { ... }") with one cFEButton per entry, each
// with its navigation (onUp="FOC:StartButton9") and action (onSelect=
// "FWD:MMOptionsMenu" opens a screen, "ACT:LMMission" goes to the screen's
// command handler).
//
// - sub_8240BD00 inflates a file of a .big into a new buffer. Once title.txt
//   is inflated we take out XBOX LIVE and add two copies of the OPTIONS button
//   below STRATEGY GUIDE (onSelect="ACT:RecompSettings" / "ACT:ExitGame"),
//   linked into the up / down order. The
//   file has to keep its length (the loader keeps the size from elsewhere: a
//   longer file got cut off), so the buttons lose their "textback_all"
//   widgets: hidden (Visible="false", A=0) and never named by the code.
// - The label is a string ID (Text="103 IDS_MM_OPTIONS"); sub_824A35D0, the
//   string table lookup, answers our unused IDs 99000 and 99001.
// - sub_82326E48 is the title screen's command handler (returns 1 if it
//   handled the command); our actions are answered there.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "main_menu_entry.h"
#include "settings_menu.h"

namespace {

// title.txt inflated (the XEX and game data are fixed: the build checks the
// XEX MD5); other files are left alone after one comparison.
constexpr uint32_t kTitleSize = 83980;
constexpr std::string_view kTitleStart = "cFEScreen TitleScreen";
// Labels: string IDs globstr.bcs doesn't have (it ends at 98009 and 99999,
// "Yeap, its always missing.").
struct Label {
  uint32_t id;
  std::string_view text;
};
constexpr Label kLabels[] = {{99000, "RECOMP SETTINGS"}, {99001, "EXIT GAME"}};
constexpr uint32_t kCommandAction = 7;  // an onSelect="ACT:..." as the handler gets it
constexpr std::string_view kRecompSettings = "RecompSettings";
constexpr std::string_view kExitGame = "ExitGame";

std::function<void()> g_quit;

// The game's name hash (sub_823FF9C8): h = h * 33 ^ c, chars sign-extended.
constexpr uint32_t NameHash(std::string_view s) {
  uint32_t h = 0;
  for (char c : s) h = h * 33 ^ uint32_t(int32_t(static_cast<signed char>(c)));
  return h;
}

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The block "<head>\n{ ... }\n" starting at or after from, as [start, end).
bool FindBlock(std::string_view text, std::string_view head, size_t from, size_t& start,
               size_t& end) {
  start = text.find(head, from);
  if (start == std::string_view::npos) return false;
  int depth = 0;
  for (size_t i = start; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}' && --depth == 0) {
      end = i + 1;
      if (end < text.size() && text[end] == '\n') ++end;
      return true;
    }
  }
  return false;
}

std::string Block(std::string_view text, std::string_view head) {
  size_t start, end;
  return FindBlock(text, head, 0, start, end) ? std::string(text.substr(start, end - start))
                                              : std::string();
}

bool ReplaceOnce(std::string& s, std::string_view from, std::string_view to) {
  const size_t at = s.find(from);
  if (at == std::string::npos) return false;
  s.replace(at, from.size(), to);
  return true;
}

// Sets the button's own Y (its first Y line).
bool SetY(std::string& button, std::string_view y) {
  const size_t at = button.find("\nY=");
  if (at == std::string::npos) return false;
  const size_t end = button.find('\n', at + 1);
  button.replace(at + 3, end - (at + 3), y);
  return true;
}

// A copy of OPTIONS (StartButton8) as another entry.
std::string NewButton(const std::string& options, std::string_view name, std::string_view up,
                      std::string_view down, std::string_view action, std::string_view y,
                      const Label& label) {
  std::string button = options;
  if (!ReplaceOnce(button, "cFEButton StartButton8\n", "cFEButton " + std::string(name) + "\n") ||
      !ReplaceOnce(button, "onUp=\"FOC:StartButton9\"", "onUp=\"FOC:" + std::string(up) + "\"") ||
      !ReplaceOnce(button, "onDown=\"FOC:StartButton10\"",
                   "onDown=\"FOC:" + std::string(down) + "\"") ||
      !ReplaceOnce(button, "onSelect=\"FWD:MMOptionsMenu\"",
                   "onSelect=\"ACT:" + std::string(action) + "\"") ||
      !SetY(button, y))
    return {};
  constexpr std::string_view kOptions = "Text=\"103 IDS_MM_OPTIONS\"";
  const std::string text = "Text=\"" + std::to_string(label.id) + " " + std::string(name) + "\"";
  size_t labels = 0;
  for (size_t at = 0; (at = button.find(kOptions, at)) != std::string::npos; ++labels)
    button.replace(at, kOptions.size(), text);
  return labels ? button : std::string();
}

// title.txt with our entries, as long as the original, or "" if the file
// isn't what we expect.
//   PLAY BALL! (StartButton1) ... EXTRA GAME MODES (11), [XBOX LIVE (9)
//   removed], OPTIONS (8), STRATEGY GUIDE (10), RECOMP SETTINGS, EXIT GAME
std::string PatchMainMenu(std::string_view original) {
  std::string text;
  size_t from = 0, start, end;
  while (FindBlock(original, "cFEWidget textback_all\n{\n", from, start, end)) {
    text += original.substr(from, start - from);
    from = end;
  }
  text += original.substr(from);

  std::string first = Block(text, "cFEButton StartButton1\n{\n");
  std::string extra = Block(text, "cFEButton StartButton11\n{\n");
  std::string online = Block(text, "cFEButton StartButton9\n{\n");
  std::string options = Block(text, "cFEButton StartButton8\n{\n");
  std::string guide = Block(text, "cFEButton StartButton10\n{\n");
  if (first.empty() || extra.empty() || online.empty() || options.empty() || guide.empty())
    return {};
  const std::string recomp = NewButton(options, "StartButtonRecomp", "StartButton10",
                                       "StartButtonExit", kRecompSettings, "0.61806",
                                       kLabels[0]);
  const std::string exit = NewButton(options, "StartButtonExit", "StartButtonRecomp",
                                     "StartButton1", kExitGame, "0.66667", kLabels[1]);
  if (recomp.empty() || exit.empty()) return {};
  const std::string old_first = first, old_extra = extra, old_options = options,
                    old_guide = guide;
  // OPTIONS and STRATEGY GUIDE move up a row (STRATEGY GUIDE's own Y is off,
  // its children move it a row down).
  if (!ReplaceOnce(first, "onUp=\"FOC:StartButton10\"", "onUp=\"FOC:StartButtonExit\"") ||
      !ReplaceOnce(extra, "onDown=\"FOC:StartButton9\"", "onDown=\"FOC:StartButton8\"") ||
      !ReplaceOnce(options, "onUp=\"FOC:StartButton9\"", "onUp=\"FOC:StartButton11\"") ||
      !SetY(options, "0.52084") ||
      !ReplaceOnce(guide, "onDown=\"FOC:StartButton1\"", "onDown=\"FOC:StartButtonRecomp\"") ||
      !SetY(guide, "0.51389"))
    return {};
  if (!ReplaceOnce(text, old_first, first) || !ReplaceOnce(text, old_extra, extra) ||
      !ReplaceOnce(text, online, "") || !ReplaceOnce(text, old_options, options) ||
      !ReplaceOnce(text, old_guide, guide + recomp + exit))
    return {};
  if (text.size() > original.size()) return {};
  text.append(original.size() - text.size(), '\n');
  return text;
}

}  // namespace

// sub_8240BD00(src, src_size, out_size*, ...) returns the new buffer.
extern "C" REX_FUNC(__imp__sub_8240BD00);
extern "C" REX_FUNC(sub_8240BD00) {
  const uint32_t out_size_ptr = ctx.r5.u32;
  __imp__sub_8240BD00(ctx, base);
  const uint32_t buffer = ctx.r3.u32;
  if (!buffer || !out_size_ptr || LoadBE32(base + out_size_ptr) != kTitleSize) return;
  char* text = reinterpret_cast<char*>(base + buffer);
  if (std::string_view(text, kTitleStart.size()) != kTitleStart) return;
  const std::string patched = PatchMainMenu(std::string_view(text, kTitleSize));
  if (patched.size() != kTitleSize) {
    REXLOG_WARN("Main menu: title.txt not as expected, left as it is");
    return;
  }
  std::memcpy(text, patched.data(), patched.size());
  REXLOG_INFO("Main menu: XBOX LIVE removed, RECOMP SETTINGS and EXIT GAME added");
}

// sub_824A35D0(table, id): the string for an ID (a guest char*).
extern "C" REX_FUNC(__imp__sub_824A35D0);
extern "C" REX_FUNC(sub_824A35D0) {
  size_t index = 0;
  while (index < std::size(kLabels) && kLabels[index].id != ctx.r4.u32) ++index;
  if (index == std::size(kLabels)) {
    [[clang::musttail]] return __imp__sub_824A35D0(ctx, base);
  }
  static std::atomic<uint32_t> strings[std::size(kLabels)] = {};
  const std::string_view text = kLabels[index].text;
  uint32_t address = strings[index].load();
  if (!address) {
    address = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(text.size() + 1));
    std::memcpy(base + address, text.data(), text.size());
    base[address + text.size()] = 0;
    uint32_t expected = 0;
    if (!strings[index].compare_exchange_strong(expected, address)) {
      REX_KERNEL_MEMORY()->SystemHeapFree(address);
      address = expected;
    }
  }
  ctx.r3.u64 = address;
}

// The title screen's command handler (this, command {type, name hash, ...}).
extern "C" REX_FUNC(__imp__sub_82326E48);
extern "C" REX_FUNC(sub_82326E48) {
  const uint8_t* command = base + ctx.r4.u32;
  if (LoadBE32(command) == kCommandAction) {
    const uint32_t name = LoadBE32(command + 4);
    if (name == NameHash(kRecompSettings)) {
      tb2::OpenSettingsMenu();
      ctx.r3.u64 = 1;
      return;
    }
    if (name == NameHash(kExitGame)) {
      if (g_quit) g_quit();
      ctx.r3.u64 = 1;
      return;
    }
  }
  [[clang::musttail]] return __imp__sub_82326E48(ctx, base);
}

namespace tb2 {

void InitMainMenuEntries(std::function<void()> quit) { g_quit = std::move(quit); }

}  // namespace tb2
