#!/bin/sh
# Native renderer phase 2: validate the SPIR-V a native capture wrote
# (native_capture_NNN_shaders/, see src/native_capture.cpp) with spirv-val, for
# Vulkan 1.2 with scalarBlockLayout (the translator's uniform blocks need it).
#
#   tools/native_validate.sh <captures dir or shader dirs...>
#
# spirv-val comes from SPIRV-Tools (SPIRV_VAL=<path> if it isn't on PATH). The
# SDK checkout has the source; build it outside the tree:
#   cmake -S ~/src/rexglue-sdk/thirdparty/spirv-tools -B /tmp/spvtools -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release -DSPIRV_SKIP_TESTS=ON \
#     -DSPIRV-Headers_SOURCE_DIR=$HOME/src/rexglue-sdk/thirdparty/spirv-headers
#   ninja -C /tmp/spvtools spirv-val
VAL="${SPIRV_VAL:-spirv-val}"
command -v "$VAL" >/dev/null 2>&1 || { echo "spirv-val not found (set SPIRV_VAL)"; exit 1; }
dirs=""
for a in "$@"; do
  case "$a" in
    *_shaders) dirs="$dirs $a" ;;
    *) dirs="$dirs $(find "$a" -maxdepth 1 -type d -name 'native_capture_*_shaders' | sort)" ;;
  esac
done
total=0; bad=0
seen=$(mktemp); trap 'rm -f "$seen"' EXIT
for d in $dirs; do
  for f in "$d"/*.spv; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    grep -qx "$name" "$seen" && continue  # same shader + modification in another capture
    echo "$name" >> "$seen"
    total=$((total + 1))
    if ! out=$("$VAL" --target-env vulkan1.2 --scalar-block-layout "$f" 2>&1); then
      bad=$((bad + 1))
      echo "FAIL $f"
      echo "$out" | head -5 | sed 's/^/  /'
    fi
  done
done
echo "$total unique SPIR-V modules, $bad failed validation"
[ "$bad" -eq 0 ]
