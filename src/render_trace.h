// thebigs2 - render call tracing (build with -DTB2_RENDER_TRACE=ON).
//
// Every recompiled function in the traced ranges (the XDK D3D library and
// the engine's renderlib) is wrapped by generated code that calls
// TB2TraceHit (hooked ones: TB2_TRACE_HOOKED). Press F8 (or `kill -USR2 <pid>` on Linux) to record the next
// frame(s): call counts, callers and the exact call sequence, written to
// <user data>/traces/. See docs/native-renderer.md.
#pragma once

#include <cstdint>
#include <filesystem>

struct TB2TraceEntry {
  uint32_t address;
  uint32_t range;
};

extern const uint32_t tb2_trace_count;
extern const TB2TraceEntry tb2_trace_entries[];
extern const char* const tb2_trace_range_names[];

void TB2TraceHit(uint32_t index, uint64_t lr);
void TB2TraceHitAddress(uint32_t address, uint64_t lr);

// Functions the game hooks itself (extern "C" REX_FUNC(sub_X) in src/) get no
// generated wrapper (it would be a second definition); their hook reports the
// call with this instead, as its first statement.
#if defined(TB2_RENDER_TRACE)
#define TB2_TRACE_HOOKED(address) TB2TraceHitAddress(address, ctx.lr)
#else
#define TB2_TRACE_HOOKED(address) ((void)0)
#endif

// Registers the F8 bind / SIGUSR2 and sets where traces are written.
void TB2TraceInit(const std::filesystem::path& out_dir);
