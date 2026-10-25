# Cross-compile for Windows x64 (MSVC ABI) from Linux with LLVM clang/lld.
#
# The MSVC CRT and Windows SDK come from xwin
# (https://github.com/Jake-Shadle/xwin):
#   xwin --accept-license --arch x86_64 splat --output <dir>
# and are found through XWIN_DIR (cache variable or environment variable;
# default ~/msvc-sdk).
#
# The driver is clang++ in GNU mode targeting x86_64-pc-windows-msvc, the same
# compiler front end the SDK's Windows presets use (clang++ on Windows).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    if(DEFINED ENV{XWIN_DIR})
        set(XWIN_DIR "$ENV{XWIN_DIR}")
    else()
        set(XWIN_DIR "$ENV{HOME}/msvc-sdk")
    endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat output (MSVC CRT + Windows SDK)")
if(NOT EXISTS "${XWIN_DIR}/crt/include")
    message(FATAL_ERROR "XWIN_DIR='${XWIN_DIR}' is not an xwin splat directory")
endif()
# try_compile projects need it too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(LLVM_SUFFIX "-20" CACHE STRING "Suffix of the LLVM tool names (clang-20, ...)")

set(_triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER   clang${LLVM_SUFFIX})
set(CMAKE_CXX_COMPILER clang++${LLVM_SUFFIX})
set(CMAKE_C_COMPILER_TARGET   ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})
set(CMAKE_RC_COMPILER llvm-rc${LLVM_SUFFIX})
set(CMAKE_MT          llvm-mt${LLVM_SUFFIX})
set(CMAKE_AR          llvm-ar${LLVM_SUFFIX})
set(CMAKE_RANLIB      llvm-ranlib${LLVM_SUFFIX})
set(CMAKE_LINKER_TYPE LLD)
# xwin ships only the release CRT (no msvcrtd.lib).
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(_inc
    "${XWIN_DIR}/crt/include"
    "${XWIN_DIR}/sdk/include/ucrt"
    "${XWIN_DIR}/sdk/include/um"
    "${XWIN_DIR}/sdk/include/shared"
    "${XWIN_DIR}/sdk/include/winrt")
set(_lib
    "${XWIN_DIR}/crt/lib/x86_64"
    "${XWIN_DIR}/sdk/lib/um/x86_64"
    "${XWIN_DIR}/sdk/lib/ucrt/x86_64")

# -idirafter (clang-cl: -imsvc) puts the MSVC headers after clang's own (intrin.h, immintrin.h, ...);
# as -isystem they'd shadow them. They're in the *_FLAGS_INIT, so don't set
# CMAKE_C(XX)_FLAGS on the command line: that replaces them. -march is the
# SDK's x86-64-v2 baseline.
set(_cflags "-march=x86-64-v2")
foreach(_d IN LISTS _inc)
    string(APPEND _cflags " -idirafter \"${_d}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT   "${_cflags}")
set(CMAKE_CXX_FLAGS_INIT "${_cflags}")
foreach(_lang C CXX)
    set(CMAKE_${_lang}_STANDARD_LINK_DIRECTORIES ${_lib})
endforeach()
set(_rcflags "")
foreach(_d IN LISTS _inc)
    string(APPEND _rcflags " -I \"${_d}\"")
endforeach()
set(CMAKE_RC_FLAGS_INIT "${_rcflags}")

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
