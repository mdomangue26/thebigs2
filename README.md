# The Bigs 2 — ReXGlue static recompilation

Native Windows/Linux port of *The Bigs 2* (Xbox 360, 2K Sports, 2009) built by
statically recompiling the original PowerPC executable to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) **`development`** branch.

No game data is included. You need your own dump of the disc.

## Layout

| Path | What it is |
|---|---|
| `thebigs2_manifest.toml` | Codegen entry point (project, XEX path, includes) |
| `config/codegen.toml` | Register/codegen flags and analysis tuning |
| `config/overrides.toml` | Hand-written function bounds, jump tables, hooks, CRT map |
| `src/thebigs2_app.h` | `ReXApp` subclass — runtime hooks go here |
| `src/hooks.cpp` | Game-specific runtime fixes (ad server, `XeCryptMd5*`, save listing) |
| `src/settings.cpp` | Settings file template, display mode, disc-image (.iso) support |
| `src/first_launch.cpp` | First-launch setup screen (ImGui): pick the disc image, extract it |
| `assets/fonts/` | Roboto (Apache 2.0) for the setup screen; Barlow Condensed (SIL OFL 1.1) for the settings menu and the F3 overlay |
| `src/frame_stats.cpp` | Frame-rate counter (F3 overlay, log) and the experimental 60 fps unlock (`frame_rate`) |
| `src/render_trace.cpp`, `src/renderdoc_capture.cpp` | Debug tooling: render-call trace build (F8), RenderDoc capture (F9) |
| `docs/native-renderer.md` | Plan and findings for a native Vulkan renderer |
| `tools/` | Analysis scripts (call graph, string refs, render-trace generator), `atbat.sh` (scripted gameplay frame for rendering checks), `perf/` (GPU command thread benchmark, gdb stack sampler) |
| `packaging/` | AppImage recipe (`build-appimage.sh`, desktop file, icon; `PROFILE=steamdeck` for the Deck); Windows cross-build (`build-windows.sh`, `windows/` toolchain file and icon) |
| `game/` | **Your** extracted disc contents (`default.xex` + data). Git-ignored. |
| `generated/` | Codegen output — regenerated, never edit by hand |

## 1. Build and install the SDK (once per machine)

Requires CMake 3.25+, Ninja, and **Clang 20+**.

**Linux (Ubuntu 24.04)**

```bash
sudo apt install clang-20 lld-20 ninja-build cmake pkg-config nasm \
  libgtk-3-dev libx11-xcb-dev libxcb1-dev libxcb-xkb-dev libxcb-randr0-dev \
  libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-xfixes0-dev libxcb-shape0-dev \
  libxss-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdrm-dev \
  libgbm-dev libudev-dev libdbus-1-dev libpipewire-0.3-dev libasound2-dev \
  libpulse-dev libvulkan-dev
git clone --recursive -b development https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
cmake --build out/build/linux-amd64 --config Release --target install
```

**Windows** — Visual Studio 2022 with *Desktop development with C++*, *C++ Clang
Compiler for Windows (20.x+)* and *MSBuild support for LLVM*. From a
*Developer PowerShell for VS 2022*:

```powershell
git clone --recursive -b development https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config Release --target install
```

(Or cross-compile the Windows build on Linux: section 6.)

Install registers the SDK with CMake, so this project finds it automatically.
To use a source checkout instead, configure with `-DREXSDK_DIR=/path/to/rexglue-sdk`.

**SDK fixes.** This project is tested against `development` @ bd833a2 plus
these fixes on a local branch (`fix/stability`, not in the upstream SDK):

| Commit | Fix | Without it |
|---|---|---|
| 43f34f9 | Unlink the guest-memory shm object right after creating it | Every crash/kill leaves a multi-GB file in `/dev/shm`; once it fills, the next launch dies (SIGBUS) |
| 5783f3d | Lock the InputSystem entry points | Heap corruption and an abort when a controller disconnects and reconnects |
| b4bf254 | Request vertex buffer ranges on every draw | Stale geometry when the game rewrites a vertex buffer in place |
| a52de02 | Sample cube maps with an LOD from the gradients | NVIDIA GPU hang at 2x (see *Rendering defaults*) |
| dfd3581 | Bulk type-0 register writes in the command processor | `frame_rate = 60` can't get past ~30 fps |
| a4b505d | Libraries pinned to older glibc symbol versions | Built on a glibc 2.43 system, nothing loads on SteamOS |
| ad05653 | 4xMSAA depth resolve reads each 8-sample half at its own address | At 1x, dashed streaks on and around players (depth-of-field fed garbage depth every 8 rows) |
| 690bf1c | RenderDoc used only when already loaded | Where RenderDoc is installed (SteamOS), every run loads it: capture overhead and its overlay text in the corner |
| aa272a5 | Multi-object waits block instead of spinning; thread names fit Linux's 15 chars | The audio thread spins ~77% of a core |
| 0330f2e | Timer queue thread blocks between timers | ~10% of a core while idle |
| 4d14346 | Frame pacing: frames presented on a fixed beat after the emulated vblank, precise vblank timer, achievement toast only registered while shown (adapted from SVR2011-Recomp's frame-pacing patch) | Uneven frame times: frames reach the screen whenever the GPU emulation finishes them |
| 87978cb | Write-watch faults no longer parse `/proc/self/maps` under the global lock | Every game write to GPU-watched memory stalls the GPU thread and other game threads (~20% of the GPU thread's frame time) |
| 8088994 | Shared memory validity checked without the global lock | The GPU thread contends with game threads on every draw |
| e806d48 | Cheaper per-draw constants and cvar check; `cp_frame_stats` | ~10% more GPU thread time per frame |
| 46f7329 | No build-stamp watermark under the F3 overlay | Debug text over the bottom of the screen while F3 is open |
| c2f9c1e | Controller button remapping (`padbind_*`) | The settings menu's Controller page and the `padbind_*` settings do nothing |

With an unpatched SDK the game still runs; use `resolution_scale = 1` and
leave `frame_rate` at 30.

## 2. Put your game files in `game/`

Extract your disc image so `game/default.xex` exists alongside the game's data
folders. On Linux, `extract-xiso -x TheBigs2.iso -d game` or
`xdvdfs unpack TheBigs2.iso game` both work. If you have a title update, put the
`.xexp` in `game/` and uncomment `patch_file_path` in `config/codegen.toml`.

## 3. Recompile and build

```bash
# Linux
cmake --preset linux-amd64-release -DCMAKE_CXX_COMPILER=clang++-20
cmake --build --preset linux-amd64-release

# Windows
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
```

> **After changing `config/codegen.toml`:** if the build fails with
> `thebigs2_pch.h has been modified since the precompiled header was built`,
> just run the build again — codegen rewrites that header mid-build.

> **First build, or after deleting `generated/default`:** run the configure +
> build pair **twice**. The build list of recompiled sources is read at
> configure time, so the first pass only produces them; the second compiles and
> links them (otherwise the link fails with `undefined reference to PPCImageConfig`).

Codegen runs automatically as part of the build (target `thebigs2_codegen`) and
re-runs only when the XEX or a config file changes.

**Where the game data comes from**, first match wins:

1. `--game_data_root <dir>` (an extracted disc).
2. A `game/` folder containing `default.xex` next to the executable (or the
   AppImage), or in the working directory.
3. A previous extraction in the user data folder
   (`~/.local/share/thebigs2/game`; for the AppImage and the Windows folder,
   `game/` next to it).
4. Otherwise the **setup screen** opens in the game window and the disc image
   you choose is extracted there once. It preselects `iso_path` from the
   settings file (or `--iso_path=...`), else a matching `.iso` next to the
   executable / AppImage.

The setup screen is a small file browser (shortcuts for the game's folder,
Home, Downloads and mounted SD cards / USB drives, or drive letters on
Windows) that checks the chosen
image right away, then shows the extraction progress, with Cancel. It works
with mouse, touchscreen, keyboard (arrows, Enter, Backspace = up a folder)
and controller (D-pad / stick, A, B = up a folder), fullscreen and in Steam
Deck Game Mode.

Full disc dumps (Redump-style, with the video partition) and XISOs both work.
The image is only accepted if its `default.xex` is byte-identical to the one
this build was recompiled from (MD5 baked in at configure time from
`game/default.xex`). Extraction goes to `game.partial/` and is renamed when
complete, so an interrupted run starts over cleanly. Once extracted, the
`.iso` is no longer needed. To re-extract, delete that `game` folder.

## 4. Settings file

Settings live in **`thebigs2.toml`**. The in-game **settings menu** (**F4**,
or hold **Back + Y** on a controller for a second) edits it: it shows what the
file sets, saves changes into the same lines (comments stay), and applies the
frame rate, upscaling and letterbox at once; the rest take effect after a
restart (*Save and quit*). It looks like the game's own menus: LB / RB (or
Page Up / Page Down) switch between Display, Graphics, Controls, Controller
and Keyboard; up / down pick a setting, left / right (or A) change it, A /
Enter edits a key. Start or Ctrl+S saves, Y saves and quits, B or F4 closes.
While it is open the game doesn't see the controller. The file itself:

- `~/.config/thebigs2/thebigs2.toml` on Linux (`%APPDATA%\thebigs2\` on Windows),
  written with comments on first launch;
- or, for a portable install, a `thebigs2.toml` placed next to the executable;
- the AppImage and the Windows build keep it in `config/` next to themselves
  (see sections 5 and 6).

| Setting | Default | |
|---|---|---|
| `iso_path` | `""` | Disc image to extract on first launch |
| `fullscreen` | `true` | `false` = windowed |
| `fullscreen_exclusive` | `false` | `false` borderless (desktop mode), `true` switches the monitor mode |
| `display_resolution` | `"desktop"` | `WIDTHxHEIGHT`: window size, or the exclusive-fullscreen mode |
| `display_refresh_rate` | `0` | Hz for exclusive fullscreen (`0` = desktop). 60/120 are smoothest |
| `monitor` | `0` | 0 = default, 1 = first, ... |
| `present_letterbox` | `true` | Keep 16:9 with bars |
| `resolution_scale` | `2` | Internal render scale: `1` = 1x (720p), `2` = 2x (1440p), `3` = 3x (4K) |
| `present_effect` | `"bilinear"` | `bilinear`, `cas`, `fsr` |
| `render_target_path_vulkan` | `"fsi"` | `fsi` or `host` |
| `anisotropic_override` | `3` | Texture filtering: `-1` = the game's own, `0` = off, `1`–`5` = forced 1x–16x |
| `gpu_backend` | `"vulkan"` | Graphics API. Windows only: `"d3d12"` is the alternative (untested, see section 6) |
| `frame_rate` | `30` | Frame rate in matches: `30` (the game's own) or `60` — **60 is experimental** (see below). Replaces `fps_60`, which still works |
| `scene_msaa` | automatic | The game's 4x MSAA on the main scene; automatic = on at 1x, off at 2x+ and on the Steam Deck (see below) |
| `fps_log_interval` | `0` | Log the frame rate every N seconds (`0` = off) |
| `bind_quit` | `"F10"` | Quit key |
| `mnk_mode` | `true` | Keyboard plays as player 1's controller (alongside the first pad) |
| `keybind_*` | see below | Keyboard bindings for each controller input |
| `padbind_*` | the button itself | Controller button remapping (see below) |

Any setting can also be passed for one run as `--name=value`.

### Keyboard controls

The keyboard is player 1's controller, merged with the first gamepad so both
work at once. Each `keybind_*` setting takes one or more keys separated by
commas, optionally with modifiers (`"Shift+Up"`); modifiers must match exactly,
which lets the arrows drive the right stick and Shift+arrows the D-pad. The
settings file lists every accepted key name.

| Controller | Default keys | Setting |
|---|---|---|
| A / B / X / Y | `;` or Space / `'` or Backspace / L / P | `keybind_a`, `_b`, `_x`, `_y` |
| LB / RB | 1 / 3 | `keybind_left_shoulder`, `keybind_right_shoulder` |
| LT / RT | Q or I / E or O | `keybind_left_trigger`, `keybind_right_trigger` |
| Back / Start | Z or Tab / X or Return | `keybind_back`, `keybind_start` |
| Left stick (click) | W A S D (F) | `keybind_lstick_up/down/left/right/press` |
| Right stick (click) | arrow keys (K) | `keybind_rstick_up/down/left/right/press` |
| D-pad | Shift + arrow keys | `keybind_dpad_up/down/left/right` |

Holding Back + Start for 2 seconds quits from the keyboard too.

### Controller buttons

Each controller button can press a different game button: `padbind_a`,
`_b`, `_x`, `_y`, `_lb`, `_rb`, `_lt`, `_rt`, `_back`, `_start`, `_ls`, `_rs`
(stick clicks), `_up`, `_down`, `_left`, `_right` (d-pad) name the game button
that physical button presses: `A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`,
`Back`, `Start`, `LS`, `RS`, `Up`, `Down`, `Left`, `Right`, or `None` to turn
it off (e.g. `padbind_a = "B"` and `padbind_b = "A"` swap A and B). A trigger
mapped to a button presses it past a light pull; a button mapped to a trigger
pulls it fully. It applies to every controller, not the keyboard (which has
its own keys), after a restart. The settings menu's Controller page edits
these.

Settings files are only written on first launch, so one from an older build
lacks newer settings. The frame rate and controller button sections are
added to such files automatically on the next launch (an old `fps_60 = true`
becomes `frame_rate = 60`); for other new settings the defaults apply, and you can add
the lines by hand or delete the file to get a fresh one with every setting and
its comments.

Notes: the game's own (guest) video mode stays 1280x720 @ 60 Hz no matter what
the display is set to — `display_resolution` deliberately doesn't use the SDK's
`resolution` / `window_width` settings, which would also change what the game
is told its TV is. On Wayland, "exclusive" modes are emulated by the compositor
and can't change the real refresh rate; set it in your desktop's display
settings, or run with `SDL_VIDEODRIVER=x11`.

## 5. AppImage (Linux)

After a release build:

```bash
packaging/build-appimage.sh      # -> out/appimage/TheBigs2-x86_64.AppImage
```

It bundles the executable, `librexruntime.so`, `libTracyClient.so` and the
Xenos GPU plugin (~13 MB) — **no game data**. Put it in its own folder, run it,
and pick your `.iso` on the setup screen (an `.iso` in the same folder is
preselected); after the extraction the game starts.

The AppImage is **portable**: everything it writes stays next to it.

```
TheBigs2-x86_64.AppImage
game/      extracted game files (or put your own extracted game here)
config/    thebigs2.toml
saves/     profile, save games, achievements
cache/     shader cache
logs/
```

Move or back up the folder as a whole. On the first run in a new folder, saves
and settings from an older non-portable install (`~/.local/share/thebigs2`,
`~/.config/thebigs2`) are copied in; the originals stay. If the folder isn't
writable, the AppImage falls back to those locations instead.

Needs glibc 2.38+ (the build script refuses binaries that need more: a
newer build machine pulls in new symbol versions, which `src/glibc_compat.h`
pins back) and a GCC 13+ libstdc++ (Ubuntu 24.04, Fedora 39, current
Arch/SteamOS or newer) and a Vulkan driver.

### Steam Deck AppImage

```bash
PROFILE=steamdeck packaging/build-appimage.sh   # -> out/appimage/TheBigs2-SteamDeck-x86_64.AppImage
```

The same build with defaults for the Deck. Its launcher sets
`TB2_PROFILE=steamdeck`, which changes the defaults and the settings file it
writes on first launch:

| Setting | Desktop | Steam Deck | Why |
|---|---|---|---|
| `resolution_scale` | `2` | `1` | The game's own 1280x720 fills the Deck's 1280x800 screen edge to edge (thin bars top and bottom), with no scaling; 2x would cost GPU time for no visible gain |
| `render_target_path_vulkan` | `"fsi"` | `"host"` | The Deck's GPU driver (RADV) has no fragment shader interlock; host render targets are also the cheaper path and are correct at 1x |
| `anisotropic_override` | `3` (4x) | `-1` (game's own) | Saves texture-sampling time on the Deck's GPU; this is what the Xbox 360 did |
| `mnk_mode` | `true` | `false` | Steam Input can send key presses from the trackpads and back buttons, which would also drive player 1 |
| `frame_rate` | `30` | `30` | 60 is experimental; on the Deck it holds 60 in gameplay and slows down during transitions |
| `scene_msaa` | off at 2x (on at 1x) | off | Without the game's 4x MSAA it also skips its 3-tile rendering: about half the CPU work of the GPU emulation and less GPU work |

It doesn't copy settings from an older desktop install (only saves), so the
Deck defaults stick. `TB2_PROFILE= ./TheBigs2-SteamDeck-x86_64.AppImage`
runs it with the desktop defaults.

On the Deck:

1. In Desktop Mode, make a folder for it (e.g. `~/Games/TheBigs2`) and put the
   AppImage in it, ideally with your `.iso` next to it (then it's preselected
   on the setup screen; press A). The setup screen also works in Game Mode,
   with the controls or the touchscreen, and can browse to an SD card.
2. *Add a Non-Steam Game to My Library*, pick the AppImage, then switch to
   Game Mode and play. The Deck's controls arrive as an Xbox pad.
3. Quit with **Back + Start held 2 seconds** (the View and Menu buttons).
   **Back + Y held a second** opens the settings menu.
4. On the Deck OLED (90 Hz), set the refresh rate to 60 Hz in the Quick
   Access performance menu, otherwise motion judders.

If it closes right away, run it from Konsole in Desktop Mode
(`cd ~/Games/TheBigs2 && ./TheBigs2-SteamDeck-x86_64.AppImage`): errors from
before the game's log starts (e.g. a missing library) only show there.

Tested on a Steam Deck (SteamOS 3.8): it runs at 30 fps, and with
`frame_rate = 60` it holds 60 in gameplay and slows down during transitions.
Use the F3 overlay or Steam's performance overlay to check the frame rate, and
the `logs/` folder if something goes wrong. The first minutes can stutter
while shaders compile (they're cached in `cache/` after that). The AppImage contains code
recompiled from the game's executable, so treat it like the game itself and
don't distribute it.

### Status (SDK `development` @ bd833a2, Title ID 54540830, XEX v0.0.0.3)

- Codegen: clean after the fixes in `config/overrides.toml` (9 tail-split leaf
  functions, 4 thunks, 1 leaf behind a wrapper, 2 compressed jump tables,
  43 vtable-only leaf functions, 8 code-built callbacks).
- Linux build: links, boots, brings up Vulkan via the Xenos GPU plugin, compiles
  shaders and presents frames. With only `default.xex` present it then faults
  on a null pointer right after failing to open its data files — expected
  without the rest of the disc.
- All 221 kernel/XAM imports exist in the SDK. Stubs to watch: `RtlUnwind`,
  `__C_specific_handler` (38 functions carry SEH handlers), `XamShowMessageBoxUIEx`.
- Runtime fixes in `src/hooks.cpp`: Massive ad-server lookup made to fail
  cleanly (was an endless fault loop on the ad thread), real `XeCryptMd5*`
  (SDK stubs), and an empty save listing treated as success (was
  "Unable To Connect To Device"). Boots, plays, saves and loads on Linux.
- Frame rate: 60 fps in menus, 30 in matches (the game's own cap); 60 in
  matches with the experimental `frame_rate = 60` (see below).
- Windows: cross-compiled on Linux (section 6); plays at 30 fps in matches
  under Wine 10 with Vulkan. Not yet run on real Windows.

### Frame pacing

The game presents with plain FIFO (vsync) by default; the SDK's own default
(mailbox/immediate) redraws the same frame 1000+ times a second and makes the
60 Hz game hitch. For the smoothest result use a 60 Hz or 120 Hz display mode,
or VRR/G-Sync (on the Steam Deck OLED: 60 Hz, not 90). Override with e.g.
`--vulkan_allow_present_mode_mailbox=true`.

Each finished frame is presented 5 ms after the emulated vblank it belongs to
(SDK `present_pace_to_guest_vblank`, `present_pacing_offset_us`), like the
console's display flip, so frames reach the screen on a fixed beat even when
the GPU emulation finishes them at varying times. `--present_pacing_stats=true`
logs the presentation intervals every 5 s.

### 60 fps (experimental)

The game runs menus at 60 fps but caps matches at 30 (it asks for a flip every
second vblank). `frame_rate = 60` lifts that cap. **This is experimental.**
On a Steam Deck it holds 60 in gameplay and slows down during transitions
(camera changes, replays). Gameplay speed is unaffected:
the game steps its simulation by measured real time. **Use it with
`resolution_scale = 1`**: at 1x a Ryzen 5600 + RTX 4060 holds 60 throughout a
match, while at 2x the FSI render path is GPU-bound at about 53 fps (44 with
`scene_msaa = true`), which paces worse than a steady 30. It needs the SDK's
command-processor fixes on `fix/stability`; without them even 1x stays near 30.

### Main scene MSAA and tiling

The game draws its main scene with 4x MSAA. A 1280x720 frame at 4x MSAA
doesn't fit the Xbox 360's 10 MB EDRAM, so it renders the scene in 3 tiles:
every frame the GPU replays the scene's commands once per 240-row band. In
emulation that triples the GPU command thread's work (~3,800 draws a frame
instead of ~1,400) and adds the MSAA and per-tile passes on the host GPU.
`scene_msaa = false` sets the scene up without MSAA (`src/render_tweaks.cpp`),
which fits EDRAM in one piece, so the tiling goes too: the command thread's
work per frame halves (10.5 -> 5.0 ms here) and the GPU renders ~20% more
frames. At 2x the resolution scaling smooths edges about as well as the MSAA
did, so it's off by default there and on the Steam Deck; at 1x edges are
visibly rougher without it, so it stays on there.

To check, press F3 for the debug overlay (shows the game's fps) or run with
`--fps_log_interval=5`: every 5 s the log gets the frame rate, the number of
long (> 20.8 ms) and short frames, the time spent waiting for the GPU, and the
busiest threads' CPU use (`game*` is the game's render thread). On the Steam
Deck add it to the game's Launch Options in Steam.

The game waits for the GPU in a busy loop (fine on the Xbox 360's six hardware
threads); a hook sleeps 0.1 ms between its polls instead
(`gpu_wait_sleep_us`, 0 = spin as before), which saves a whole CPU core - on
the Deck, CPU and GPU share one power budget.

### Rendering defaults

The game renders at **2x** (`--resolution_scale=2`, clean edges at 1440p)
through the **fragment-shader-interlock EDRAM path**
(`--render_target_path_vulkan=fsi`). The default host-render-target path
breaks the game's 2xMSAA targets at 2x (green, striped bat/shoes/cap). GPUs
without `fragmentShaderSampleInterlock` (e.g. AMD RADV, Steam Deck) fall back to
host render targets automatically — run those at `--resolution_scale=1`.

On NVIDIA, the SDK's `development` branch (bd833a2) hangs the GPU at 2x within
a few minutes (kernel log: `Xid 109 ... CTX SWITCH TIMEOUT`). The player
shaders sample a cube map with explicit gradients after a per-pixel branch,
which the driver doesn't survive. The fix (cube maps sampled with an LOD
computed from the gradients, setting `spirv_cube_lod_from_gradients`) is
SDK commit a52de02 (see *SDK fixes* in section 1); with an unpatched SDK use
`--resolution_scale=1`.

### Quitting and hotkeys

Press **F10**, or hold **Back + Start** on a controller for 2 seconds. Both
close the window through the normal shutdown. F10 can be rebound with the
`bind_quit` setting (e.g. `--bind_quit=Escape`).

| Key | Does |
|---|---|
| F10 | Quit (`bind_quit`) |
| F3 | SDK debug overlay, including the game's frame rate |
| F4 | Settings menu (`bind_settings_menu`; controller: hold Back + Y). Replaces the SDK's generic settings overlay, whose "Save to config" rewrote thebigs2.toml without its comments |
| F7 / Backtick | SDK achievements and console overlays |
| F9 | Capture frames in RenderDoc (only when launched from RenderDoc) |
| F8 | Record a render trace (trace build only, `-DTB2_RENDER_TRACE=ON`) |

## 6. Windows build (cross-compiled on Linux)

```bash
packaging/build-windows.sh      # -> out/windows/TheBigs2-windows-x64.zip
```

The zip holds `thebigs2.exe`, `rexruntime.dll`, the Xenos GPU plugin
`rexgpu-xenos.dll` and the Visual C++ runtime DLLs (~15 MB zipped) — **no
game data**. Unzip it to a folder of its own, run `thebigs2.exe` and pick your
`.iso` on the setup screen, as with the AppImage. It is portable the same way:
`game/`, `config/`, `saves/`, `cache/` and `logs/` are created next to the
exe. If the folder isn't writable (e.g. under `Program Files`), it uses
`%APPDATA%\thebigs2` and the SDK's user data folder instead.

Needs 64-bit Windows 10/11 and a current GPU driver with Vulkan (NVIDIA, AMD
or Intel). It renders with **Vulkan**, like Linux, so the
same settings and SDK fixes apply; `gpu_backend = "d3d12"` switches to the
SDK's Direct3D 12 renderer, which is untested here (it lacks the 4x MSAA
resolve fix, and under Wine it draws a black screen).

**Why the Visual C++ DLLs are bundled.** The build uses the MSVC 14.44
headers, whose `std::mutex` / `condition_variable` need `msvcp140.dll` 14.40
or newer. With an older one (an outdated VC++ redistributable, or Wine's
builtin), a condition-variable wait doesn't release its mutex: the vblank
thread then blocks on the GPU thread every frame and matches crawl at 1–2 fps
(menus look fine). Windows loads the DLLs next to the exe first. Under Wine
they are only used with
`WINEDLLOVERRIDES="msvcp140,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n"`.

**Building it.** Everything runs on Linux with the same clang 20:

1. The MSVC CRT and Windows SDK headers/libraries, via
   [xwin](https://github.com/Jake-Shadle/xwin) (accepts Microsoft's license):
   ```bash
   xwin --accept-license --arch x86_64 splat --output /mnt/Games/Non-Steam/msvc-sdk
   ```
   `packaging/windows/clang-msvc-cross.cmake` looks there, or in `XWIN_DIR`.
   Two headers are included with a different case than xwin writes them; add
   `ln -s objbase.h sdk/include/um/ObjBase.h` and
   `ln -s sdkddkver.h sdk/include/shared/SDKDDKVer.h` in that folder.
2. The SDK (`fix/stability`) for Windows, installed to `~/rexsdk-install-win`
   (Vulkan and D3D12 both on):
   ```bash
   cd ~/src/rexglue-sdk-fix
   cmake -S . -B out/build/win-amd64-cross -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_TOOLCHAIN_FILE=/mnt/Games/Non-Steam/thebigs2/packaging/windows/clang-msvc-cross.cmake \
     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON \
     -DCMAKE_INSTALL_PREFIX=$HOME/rexsdk-install-win
   cmake --build out/build/win-amd64-cross && cmake --install out/build/win-amd64-cross
   rm ~/.cmake/packages/rexglue/$(echo -n ~/rexsdk-install-win | md5sum | cut -c1-32)
   ```
   The last line takes the Windows SDK back out of CMake's package registry,
   where the install puts it; otherwise a Linux configure could pick it up.
   Don't set `CMAKE_C(XX)_FLAGS` on the command line: the toolchain file's
   include paths are in their defaults.
3. `packaging/build-windows.sh` configures `out/build/win-amd64-cross`, builds
   (codegen runs with the Linux SDK's `rexglue`), downloads the VC++
   redistributable from Microsoft once to take the DLLs out of it
   (`cabextract`), checks that every DLL the binaries import is bundled or
   part of Windows, and zips the folder.

To test under Wine: `WRAP=wine BIN=./out/build/win-amd64-cross/thebigs2.exe
tools/atbat.sh <outdir>` with the `WINEDLLOVERRIDES` above (copy the four
DLLs from `out/windows/TheBigs2-windows-x64/` into the build folder first).

## 7. The iteration loop

This is not push-button. Expect the first codegen run to report validation
errors. The loop is:

1. Run `rexglue codegen thebigs2_manifest.toml` and read the errors.
2. Fix them in `config/overrides.toml` — function sizes/parents, `[[switch_tables]]`,
   `[[invalid_instructions]]`. Use `--force` to emit output anyway and keep moving.
3. Build, run, and watch the log for unimplemented kernel/XAM imports; stub or
   implement them (see the SDK wiki: *Function Overrides*, *Kernel State & Objects*).
4. Once it plays, turn on the `*_as_local` flags in `config/codegen.toml` one at
   a time for performance, re-testing after each.
