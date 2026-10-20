// thebigs2 - native renderer phase 1: shadow capture (F6). See
// native_capture.cpp and docs/native-renderer.md.
#pragma once

#include <cstdint>
#include <filesystem>

namespace tb2 {

// Registers the F6 bind (and SIGRTMIN+1 on Linux); captures go to out_dir.
void NativeCaptureInit(const std::filesystem::path& out_dir);

// Called from the D3D Swap hook (frame boundary), before and after the swap.
// front_buffer: the texture object the swap shows (Swap's second argument).
void NativeCaptureOnSwap(uint8_t* base, uint32_t front_buffer);
void NativeCaptureAfterSwap(uint8_t* base);

// The game waits for the GPU (D3D's poll loop): the native renderer's worker
// has to catch up first, as it reads guest memory the game may then reuse.
void NativeWaitForRenderer();

// Releases the native renderer (app shutdown, before the SDK's teardown).
void NativeCaptureShutdown();

}  // namespace tb2
