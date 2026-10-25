# Building The Bigs 2

Everything is built on **Linux**: the Linux and Steam Deck AppImages, and the
Windows zip (cross-compiled). Tested on Ubuntu 24.04 with Clang 20.

Overview:

1. Build and install the ReXGlue SDK.
2. Put your extracted game files in `game/`.
3. Build the game (the recompiler runs as part of the build).
4. Package it: AppImage, Steam Deck AppImage, Windows zip.

## 1. The ReXGlue SDK

Requires CMake 3.25+, Ninja and **Clang 20+**.

```bash
sudo apt install clang-20 lld-20 ninja-build cmake pkg-config nasm \
  libgtk-3-dev libx11-xcb-dev libxcb1-dev libxcb-xkb-dev libxcb-randr0-dev \
  libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-xfixes0-dev libxcb-shape0-dev \
  libxss-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdrm-dev \
  libgbm-dev libudev-dev libdbus-1-dev libpipewire-0.3-dev libasound2-dev \
  libpulse-dev libvulkan-dev curl cabextract
git clone --recursive -b fix/stability https://github.com/mdomangue26/rexglue-sdk.git
cd rexglue-sdk
cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
cmake --build out/build/linux-amd64 --config Release
cmake --install out/build/linux-amd64 --config Release --prefix ~/rexsdk-install
```

The game's build finds the SDK in `~/rexsdk-install`, or wherever CMake's
package registry points; to use another location, configure the game with
`-DREXSDK_DIR=/path/to/sdk`.

### SDK fixes

The SDK above is the port's own fork: the upstream SDK's `development` branch
at **bd833a2** plus the fixes below, on the branch **`fix/stability`**
([mdomangue26/rexglue-sdk](https://github.com/mdomangue26/rexglue-sdk/tree/fix/stability)).
Use it. The upstream SDK also builds the game, which then runs, but without
these fixes: use `resolution_scale = 1` and keep `frame_rate` at 30 there.

| Fix | Without it |
|---|---|
| Unlink the guest-memory shm object right after creating it | Every crash leaves a multi-GB file in `/dev/shm`; once it fills, the next launch dies |
| Lock the input system entry points | Crash when a controller disconnects and reconnects |
| Request vertex buffer ranges on every draw | Stale geometry |
| Sample cube maps with an LOD from the gradients | NVIDIA GPU hang at 2x |
| Bulk register writes in the command processor | `frame_rate = 60` can't get past ~30 fps |
| Libraries pinned to older glibc symbol versions | Built on a new distro, it doesn't run on SteamOS |
| 4xMSAA depth resolve fix | Dashed streaks on players at 1x |
| RenderDoc only used when already loaded | On SteamOS every run loads RenderDoc |
| Blocking waits in WaitMultiple and the timer queue | Threads spin, wasting CPU |
| Frame pacing | Uneven frame times |
| Write-watch and shared-memory locking | GPU thread stalls on game writes |
| Cheaper per-draw constants | ~10% more GPU thread time |
| No build stamp under the F3 overlay | Debug text over the bottom of the screen |
| Controller button remapping | The settings menu's Controller tab does nothing |

## 2. Game files

Extract your disc image so that `game/default.xex` exists next to the game's
data folders:

```bash
extract-xiso -x TheBigs2.iso -d game
# or: xdvdfs unpack TheBigs2.iso game
```

Full disc dumps (Redump-style) and XISOs both work. The port is made for
Title ID 54540830, XEX version 0.0.0.3. The build records the MD5 of this
`default.xex`, and the setup screen only accepts disc images with the same
one.

The `game/` folder is git-ignored. Never commit game files or
`generated/default` (code generated from the game).

## 3. Build the game

```bash
cmake --preset linux-amd64-release -DCMAKE_CXX_COMPILER=clang++-20
cmake --build --preset linux-amd64-release
./out/build/linux-amd64-release/thebigs2
```

- **On the first build, run both commands twice.** The first pass generates
  the recompiled sources, and the list of them is read at configure time.
  Otherwise the link fails with `undefined reference to PPCImageConfig`.
- If the build stops with `thebigs2_pch.h has been modified since the
  precompiled header was built`, run the build again.
- Recompilation reruns automatically when `game/default.xex` or a file in
  `config/` changes.

Logs are in `out/build/linux-amd64-release/logs/`.

## 4. Packaging

### Linux AppImage

```bash
packaging/build-appimage.sh                     # out/appimage/TheBigs2-x86_64.AppImage
PROFILE=steamdeck packaging/build-appimage.sh   # out/appimage/TheBigs2-SteamDeck-x86_64.AppImage
```

The script downloads `appimagetool` the first time. It refuses binaries that
need a glibc newer than 2.38, so the AppImage runs on older distros and
SteamOS. Building on a newer distro works: `src/glibc_compat.h` and the SDK's
glibc fix pin the symbol versions down.

The Steam Deck AppImage is the same binary; its launcher sets
`TB2_PROFILE=steamdeck`, which picks Deck defaults (1x, host render targets,
the game's own texture filtering, no keyboard as player 1, the game's MSAA
off).

### Windows zip (cross-compiled on Linux)

```bash
packaging/build-windows.sh      # out/windows/TheBigs2-windows-x64.zip
```

Before the first run:

1. **The MSVC runtime and Windows SDK** via
   [xwin](https://github.com/Jake-Shadle/xwin) (this accepts Microsoft's
   license):
   ```bash
   xwin --accept-license --arch x86_64 splat --output ~/msvc-sdk
   cd ~/msvc-sdk
   ln -s objbase.h sdk/include/um/ObjBase.h
   ln -s sdkddkver.h sdk/include/shared/SDKDDKVer.h
   ```
   The build looks in `~/msvc-sdk`; set `XWIN_DIR` to use another folder.
2. **The SDK built for Windows**, installed to `~/rexsdk-install-win`:
   ```bash
   cd rexglue-sdk
   cmake -S . -B out/build/win-amd64-cross -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_TOOLCHAIN_FILE=/path/to/thebigs2/packaging/windows/clang-msvc-cross.cmake \
     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON \
     -DCMAKE_INSTALL_PREFIX=$HOME/rexsdk-install-win
   cmake --build out/build/win-amd64-cross && cmake --install out/build/win-amd64-cross
   rm ~/.cmake/packages/rexglue/$(echo -n ~/rexsdk-install-win | md5sum | cut -c1-32)
   ```
   The last line removes the Windows SDK from CMake's package registry, so a
   Linux build doesn't pick it up. Don't pass `CMAKE_C(XX)_FLAGS`: the
   toolchain file keeps its include paths there.

The script recompiles with the Linux SDK's `rexglue` and bundles the Visual
C++ runtime DLLs (downloaded from Microsoft once, unpacked with `cabextract`).
Those DLLs are needed: with an older `msvcp140.dll`, matches run at 1-2 fps.
Under Wine, use them with
`WINEDLLOVERRIDES="msvcp140,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n"`.

The `win-amd64-release` preset is for building natively on Windows with
Visual Studio 2022 and its Clang tools. It hasn't been tested.

## Project layout

| Path | What it is |
|---|---|
| `thebigs2_manifest.toml` | Recompiler entry point |
| `config/codegen.toml`, `config/overrides.toml` | Recompiler settings and hand-written fixes (function bounds, jump tables) |
| `src/` | The port's own code: app setup, game fixes (`hooks.cpp`), rendering tweaks, settings and the settings menu, main menu entries, setup screen |
| `assets/fonts/` | Roboto (Apache 2.0), Barlow Condensed (SIL OFL 1.1) |
| `packaging/` | AppImage and Windows packaging |
| `tools/` | Analysis scripts and test scripts (`atbat.sh` plays to the first at-bat for screenshots, `perf/` benchmarks) |
| `game/` | Your extracted game files (git-ignored) |
| `generated/default` | Recompiler output (regenerated, git-ignored) |

`config/codegen.toml` has two settings that must stay `false`: `skip_msr` and
`non_argument_as_local`. Turning either on makes the game hang.
