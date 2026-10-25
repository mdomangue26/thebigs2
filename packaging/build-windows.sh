#!/usr/bin/env bash
# Cross-compile the Windows x64 build on Linux and zip it.
#
#   packaging/build-windows.sh                 # -> out/windows/TheBigs2-windows-x64.zip
#
# Needs:
#   - clang-20 / lld / llvm-rc-20 (the Linux build's toolchain)
#   - the MSVC CRT + Windows SDK from xwin, in XWIN_DIR (default ~/msvc-sdk)
#     (xwin --accept-license --arch x86_64 splat --output <dir>)
#   - the ReXGlue SDK built for Windows with the same toolchain, in
#     REXSDK_WIN (default ~/rexsdk-install-win; see docs/BUILDING.md)
#   - the host (Linux) SDK's rexglue for codegen (~/rexsdk-install/bin)
#   - cabextract, zip
#
# The zip holds the executable, the ReXGlue runtime and GPU plugin, and the
# Visual C++ runtime DLLs (app-local; see below), but no game data: on first
# launch it asks for your disc image (.iso).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/build/win-amd64-cross}"
OUT_DIR="${OUT_DIR:-$ROOT/out/windows}"
REXSDK_WIN="${REXSDK_WIN:-$HOME/rexsdk-install-win}"
NAME=TheBigs2-windows-x64
STAGE="$OUT_DIR/$NAME"
VCREDIST_URL="https://aka.ms/vs/17/release/vc_redist.x64.exe"
VCRT_DLLS="msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll"

[ -f "$REXSDK_WIN/lib/cmake/rexglue/rexglueConfig.cmake" ] ||
  { echo "No Windows ReXGlue SDK at $REXSDK_WIN (set REXSDK_WIN)." >&2; exit 1; }

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging/windows/clang-msvc-cross.cmake" \
    -DCMAKE_PREFIX_PATH="$REXSDK_WIN" -Wno-dev
fi
cmake --build "$BUILD_DIR"

# The Visual C++ runtime, next to the exe. The MSVC 14.44 STL headers inline
# parts of std::mutex / condition_variable that need msvcp140.dll 14.40 or
# newer; with an older one (an outdated redistributable, Wine's builtin)
# condition variable waits don't release their mutex and the game crawls at
# 1-2 fps. Windows loads these from the exe's folder first; Microsoft allows
# this app-local deployment of the redistributable's DLLs.
VCRT="$OUT_DIR/vcredist"
if [ ! -f "$VCRT/msvcp140.dll" ]; then
  echo "Downloading the Visual C++ redistributable..."
  rm -rf "$VCRT" && mkdir -p "$VCRT/x"
  curl -fL --retry 3 -o "$VCRT/vc_redist.x64.exe" "$VCREDIST_URL"
  cabextract -q -d "$VCRT/x" "$VCRT/vc_redist.x64.exe"
  # The burn bundle's payloads are cabinets; the x64 runtime DLLs are named
  # <dll>_amd64 in one of them.
  for cab in "$VCRT"/x/a*; do cabextract -q -d "$VCRT/x/c" "$cab" 2>/dev/null || true; done
  for dll in $VCRT_DLLS; do
    cp "$VCRT/x/c/${dll}_amd64" "$VCRT/$dll"
  done
  rm -rf "$VCRT/x" "$VCRT/vc_redist.x64.exe"
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BUILD_DIR/thebigs2.exe" "$BUILD_DIR/rexruntime.dll" "$BUILD_DIR/rexgpu-xenos.dll" "$STAGE/"
for dll in $VCRT_DLLS; do cp "$VCRT/$dll" "$STAGE/"; done
[ -f "$BUILD_DIR/gamecontrollerdb.txt" ] && cp "$BUILD_DIR/gamecontrollerdb.txt" "$STAGE/"

# Every DLL the binaries import must be in the folder or part of Windows.
for f in "$STAGE"/*.exe "$STAGE"/*.dll; do
  llvm-readobj-20 --coff-imports "$f" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/Ip' | while read -r dll; do
    lower=$(echo "$dll" | tr 'A-Z' 'a-z')
    case "$lower" in
      api-ms-win-*|kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|\
      oleaut32.dll|imm32.dll|setupapi.dll|version.dll|winmm.dll|ws2_32.dll|bcrypt.dll|\
      dxgi.dll|d3d12.dll|cfgmgr32.dll|hid.dll|dinput8.dll|xinput1_4.dll|uuid.dll) ;;
      *) [ -f "$STAGE/$lower" ] || ls "$STAGE" | grep -qix "$dll" ||
           { echo "$(basename "$f") imports $dll, which isn't in the package" >&2; exit 1; } ;;
    esac
  done
done

rm -f "$OUT_DIR/$NAME.zip"
(cd "$OUT_DIR" && zip -qr9 "$NAME.zip" "$NAME")
echo
echo "Built $OUT_DIR/$NAME.zip"
