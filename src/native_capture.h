// thebigs2 - native renderer phase 1: shadow capture (F6). See
// native_capture.cpp and docs/native-renderer.md.
#pragma once

#include <cstdint>
#include <filesystem>

namespace tb2 {

// Registers the F6 bind (and SIGRTMIN+1 on Linux); captures go to out_dir.
void NativeCaptureInit(const std::filesystem::path& out_dir);

// Called from the D3D Swap hook (frame boundary), before and after the swap.
void NativeCaptureOnSwap(uint8_t* base);
void NativeCaptureAfterSwap(uint8_t* base);

}  // namespace tb2
