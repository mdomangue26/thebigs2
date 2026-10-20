// thebigs2 - frame rate: counts the game's swaps (sub_823BA340, the D3D
// Present that calls VdSwap) for the F3 debug overlay and the log, and lifts
// the game's 30 fps cap in matches.

#include "frame_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "native_capture.h"
#include "render_trace.h"

REXCVAR_DEFINE_DOUBLE(fps_log_interval, 0.0, "The Bigs 2/Debug",
                      "Log the game's frame rate every N seconds; 0 = off");

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
Clock::time_point g_last{};
uint64_t g_frames = 0;
double g_ema_ms = 0;  // smoothed frame time for the overlay

// Log window
Clock::time_point g_window_start{};
uint64_t g_window_frames = 0;
double g_window_max_ms = 0;
double g_window_min_ms = 0;
double g_window_sum_ms = 0, g_window_sum_sq = 0;  // for the frame time spread
uint32_t g_window_long = 0;   // frames over 1.25x the average frame time
uint32_t g_window_short = 0;  // frames under 0.75x - the catch-up after a long one
// Time inside the swap call: D3D blocks there until the GPU has finished the
// previous frame (which itself waits for its vblank), so this is idle time.
double g_window_swap_ms = 0;
Clock::time_point g_swap_start{};
std::atomic<int> g_swap_tid{0};

#if defined(__linux__)
// Per-thread CPU use since the last call, busiest first ("name 87%"), from
// /proc/self/task. 100% = one core. The thread calling the game's swap is
// marked "game*" (its render thread).
std::string ThreadCpuReport(double secs) {
  static std::unordered_map<int, uint64_t> last_ticks;
  static const double ticks_per_sec = double(sysconf(_SC_CLK_TCK));
  std::vector<std::pair<double, std::string>> rows;
  double total = 0;
  if (DIR* dir = opendir("/proc/self/task")) {
    while (dirent* e = readdir(dir)) {
      const int tid = std::atoi(e->d_name);
      if (tid <= 0) continue;
      std::ifstream in(std::string("/proc/self/task/") + e->d_name + "/stat");
      std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const size_t open = stat.find('('), close = stat.rfind(')');
      if (open == std::string::npos || close == std::string::npos) continue;
      std::string name = stat.substr(open + 1, close - open - 1);
      std::istringstream rest(stat.substr(close + 2));
      std::string field;
      uint64_t utime = 0, stime = 0;
      for (int i = 3; i <= 15 && rest >> field; ++i) {
        if (i == 14) utime = std::stoull(field);
        if (i == 15) stime = std::stoull(field);
      }
      const uint64_t ticks = utime + stime;
      auto [it, inserted] = last_ticks.try_emplace(tid, ticks);
      const double pct = inserted ? 0 : 100.0 * double(ticks - it->second) / ticks_per_sec / secs;
      it->second = ticks;
      total += pct;
      if (tid == g_swap_tid.load(std::memory_order_relaxed)) name = "game*";
      if (pct >= 1.0) rows.emplace_back(pct, name);
    }
    closedir(dir);
  }
  std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string out = fmt::format("cpu {:.0f}%:", total);
  for (size_t i = 0; i < rows.size() && i < 8; ++i) {
    out += fmt::format(" {} {:.0f}%", rows[i].second, rows[i].first);
  }
  return out;
}
#endif

void OnSwapReturn() {
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  g_window_swap_ms += std::chrono::duration<double, std::milli>(now - g_swap_start).count();
}

void OnSwap() {
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  g_swap_start = now;
  ++g_frames;
#if defined(__linux__)
  if (g_swap_tid.load(std::memory_order_relaxed) == 0) {
    g_swap_tid.store(int(gettid()), std::memory_order_relaxed);
  }
#endif
  if (g_last != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - g_last).count();
    g_ema_ms = g_ema_ms == 0 ? ms : g_ema_ms * 0.95 + ms * 0.05;
    g_window_max_ms = std::max(g_window_max_ms, ms);
    g_window_min_ms = g_window_frames == 0 ? ms : std::min(g_window_min_ms, ms);
    g_window_sum_ms += ms;
    g_window_sum_sq += ms * ms;
    if (g_ema_ms > 0) {
      g_window_long += ms > g_ema_ms * 1.25;
      g_window_short += ms < g_ema_ms * 0.75;
    }
    ++g_window_frames;
  } else {
    g_window_start = now;
  }
  g_last = now;

  const double interval = REXCVAR_GET(fps_log_interval);
  if (interval <= 0) return;
  const double secs = std::chrono::duration<double>(now - g_window_start).count();
  if (secs >= interval && g_window_frames > 0) {
    const double mean = g_window_sum_ms / g_window_frames;
    const double stddev = std::sqrt(std::max(0.0, g_window_sum_sq / g_window_frames - mean * mean));
    REXLOG_INFO(
        "[fps] {:.1f} fps over {:.1f}s (frame {:.1f} +- {:.2f} ms, {:.1f}-{:.1f}, {} long (>1.25x avg), "
        "{} short (<0.75x), {:.1f} ms of it in swap)",
        g_window_frames / secs, secs, mean, stddev, g_window_min_ms, g_window_max_ms,
        g_window_long, g_window_short, g_window_swap_ms / g_window_frames);
#if defined(__linux__)
    REXLOG_INFO("[fps] {}", ThreadCpuReport(secs));
#endif
    g_window_start = now;
    g_window_frames = 0;
    g_window_max_ms = 0;
    g_window_swap_ms = 0;
    g_window_long = 0;
    g_window_short = 0;
    g_window_sum_ms = 0;
    g_window_sum_sq = 0;
  }
}

}  // namespace

namespace tb2 {

rex::ui::FrameStats GuestFrameStats() {
  std::lock_guard lock(g_mutex);
  rex::ui::FrameStats stats;
  stats.frame_count = g_frames;
  stats.frame_time_ms = g_ema_ms;
  stats.fps = g_ema_ms > 0 ? 1000.0 / g_ema_ms : 0;
  return stats;
}

}  // namespace tb2

extern "C" REX_FUNC(__imp__sub_823BA340);
extern "C" REX_FUNC(sub_823BA340) {
  TB2_TRACE_HOOKED(0x823BA340);
  tb2::NativeCaptureOnSwap(base, ctx.r4.u32);
  OnSwap();
  __imp__sub_823BA340(ctx, base);
  tb2::NativeCaptureAfterSwap(base);
  OnSwapReturn();
}

// sub_823B3310: one iteration of D3D's wait-for-GPU loop (sub_823B2160 calls
// it until the GPU's fence catches up). It busy-waits (db16cyc pauses, then
// re-reads the fence and checks a hang timeout) - fine on the 360's six
// hardware threads, but here it kept the render thread at 100% of a core for
// the whole GPU wait, ~15 of 16.7 ms per frame. On the Steam Deck that CPU
// power comes out of the GPU's share of the 15 W budget. Returns nonzero to
// keep waiting: sleep a little before the next poll.
REXCVAR_DEFINE_INT32(gpu_wait_sleep_us, 100, "The Bigs 2/Debug",
                     "Sleep between polls while the game waits for the GPU (us); 0 = spin");
extern "C" REX_FUNC(__imp__sub_823B3310);
extern "C" REX_FUNC(sub_823B3310) {
  TB2_TRACE_HOOKED(0x823B3310);
  __imp__sub_823B3310(ctx, base);
  const int32_t sleep_us = REXCVAR_GET(gpu_wait_sleep_us);
  if (ctx.r3.u32 != 0 && sleep_us > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
}

// ---------------------------------------------------------------------------
// sub_823B9CB8: the XDK D3D library queues a flip for the vblank handler.
// Its argument packs the frontbuffer address (bits 12+), the presentation
// interval (bits 8-11: minimum vblanks since the previous flip) and a tearing
// threshold (low byte). In matches the game asks for interval 2 (30 fps);
// menus already use 1. The game simulation steps by measured real time (the
// update in sub_8222DF10 gets the timer delta), so 60 fps doesn't speed it up.
// 60 is experimental: holds 60 at 1x on a desktop and in Steam Deck gameplay
// (slower in transitions); at 2x the FSI path is GPU-bound at ~53 fps.
REXCVAR_DEFINE_INT32(frame_rate, 30, "The Bigs 2",
                     "Frame rate in matches: 30 (the game's own cap) or 60 (experimental)");
// Older name, still honoured (true = frame_rate 60).
REXCVAR_DEFINE_BOOL(fps_60, false, "The Bigs 2", "Old name for frame_rate = 60");

extern "C" REX_FUNC(__imp__sub_823B9CB8);
extern "C" REX_FUNC(sub_823B9CB8) {
  TB2_TRACE_HOOKED(0x823B9CB8);
  const uint32_t arg = ctx.r3.u32;
  const uint32_t interval = (arg >> 8) & 0xF;
  static std::atomic<uint32_t> last_interval{~0u};
  if (last_interval.exchange(interval, std::memory_order_relaxed) != interval) {
    REXLOG_INFO("[fps] game present interval {} (arg {:08X})", interval, arg);
  }
  if (interval > 1 && (REXCVAR_GET(frame_rate) >= 60 || REXCVAR_GET(fps_60))) {
    ctx.r3.u64 = (arg & ~0xF00u) | (1u << 8);
  }
  [[clang::musttail]] return __imp__sub_823B9CB8(ctx, base);
}
