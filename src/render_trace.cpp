// thebigs2 - render call tracing. See render_trace.h.

#include "render_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

REXCVAR_DEFINE_INT32(trace_frames, 1, "The Bigs 2/Debug",
                     "Frames recorded per render trace (F8)");
REXCVAR_DEFINE_DOUBLE(trace_max_seconds, 5.0, "The Bigs 2/Debug",
                      "Stop a render trace after this long even if no frame ended");
REXCVAR_DEFINE_BOOL(trace_at_start, false, "The Bigs 2/Debug",
                    "Start recording a render trace at launch (boot sequence)");
REXCVAR_DEFINE_STRING(trace_frame_marker, "823BA340", "The Bigs 2/Debug",
                      "Guest function whose call ends a frame (the D3D swap)");

namespace {

enum class State : int { kIdle, kArmed, kCapturing };

struct Event {
  uint32_t index;
  uint32_t lr;
  uint16_t thread;
  uint16_t frame;
};

std::atomic<State> g_state{State::kIdle};
std::mutex g_mutex;
std::filesystem::path g_out_dir;
std::vector<Event> g_events;
std::unordered_map<uint64_t, uint32_t> g_call_sites;  // (index << 32 | lr) -> n
std::vector<uint32_t> g_counts;
std::unordered_map<std::thread::id, uint16_t> g_threads;
uint32_t g_marker_index = UINT32_MAX;
int g_frames_left = 0;
uint16_t g_frame = 0;
uint32_t g_trace_serial = 0;
std::chrono::steady_clock::time_point g_capture_start;
constexpr size_t kMaxEvents = 2'000'000;

uint32_t FindIndex(uint32_t address) {
  auto* begin = tb2_trace_entries;
  auto* end = tb2_trace_entries + tb2_trace_count;
  auto* it = std::lower_bound(begin, end, address,
                              [](const TB2TraceEntry& e, uint32_t a) { return e.address < a; });
  return (it != end && it->address == address) ? uint32_t(it - begin) : UINT32_MAX;
}

std::string RangeOf(uint32_t index) {
  return tb2_trace_range_names[tb2_trace_entries[index].range];
}

void WriteTrace() {
  std::error_code ec;
  std::filesystem::create_directories(g_out_dir, ec);
  const auto path = g_out_dir / fmt::format("render_trace_{:03}.txt", ++g_trace_serial);
  std::ofstream out(path);
  out << fmt::format("# The Bigs 2 render trace: {} frame(s), {} calls{}, {} thread(s)\n",
                     g_frame, g_events.size(), g_events.size() >= kMaxEvents ? " (truncated)" : "",
                     g_threads.size());

  // Per function: total calls and call sites, busiest first.
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < g_counts.size(); ++i)
    if (g_counts[i]) order.push_back(i);
  std::sort(order.begin(), order.end(),
            [](uint32_t a, uint32_t b) { return g_counts[a] > g_counts[b]; });
  std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> sites;
  for (auto& [key, n] : g_call_sites) sites[uint32_t(key >> 32)].emplace_back(uint32_t(key), n);

  out << "\n## functions (range, address, calls, call sites as return address x count)\n";
  for (uint32_t i : order) {
    out << fmt::format("{:<10} {:08X} {:>8}", RangeOf(i), tb2_trace_entries[i].address,
                       g_counts[i]);
    auto& s = sites[i];
    std::sort(s.begin(), s.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t k = 0; k < s.size() && k < 12; ++k)
      out << fmt::format("  {:08X}x{}", s[k].first, s[k].second);
    if (s.size() > 12) out << fmt::format("  (+{} more)", s.size() - 12);
    out << '\n';
  }

  out << "\n## sequence (frame, thread, range, function, return address)\n";
  for (const Event& e : g_events) {
    out << fmt::format("{} t{} {:<10} {:08X} {:08X}\n", e.frame, e.thread, RangeOf(e.index),
                       tb2_trace_entries[e.index].address, e.lr);
  }
  REXLOG_INFO("[trace] wrote {} ({} calls)", path.string(), g_events.size());
}

void BeginCapture() {
  g_events.clear();
  g_call_sites.clear();
  g_counts.assign(tb2_trace_count, 0);
  g_threads.clear();
  g_frame = 0;
  g_frames_left = std::max(1, REXCVAR_GET(trace_frames));
  g_capture_start = std::chrono::steady_clock::now();
  g_state.store(State::kCapturing);
}

void Arm() {
  if (tb2_trace_count == 0 || g_marker_index == UINT32_MAX) {
    REXLOG_WARN("[trace] no frame marker function; set trace_frame_marker");
    return;
  }
  State expected = State::kIdle;
  if (g_state.compare_exchange_strong(expected, State::kArmed))
    REXLOG_INFO("[trace] armed: recording from the next frame");
}

#if !defined(_WIN32)
std::atomic<bool> g_signal{false};
void OnSigUsr2(int) { g_signal.store(true); }
#endif

}  // namespace

void TB2TraceHit(uint32_t index, uint64_t lr) {
  State state = g_state.load(std::memory_order_relaxed);
  if (state == State::kIdle) return;

  std::lock_guard lock(g_mutex);
  state = g_state.load(std::memory_order_relaxed);
  if (index == g_marker_index) {
    if (state == State::kArmed) {
      BeginCapture();
      return;  // start counting after this swap
    }
    if (state == State::kCapturing && ++g_frame >= g_frames_left) {
      WriteTrace();
      g_state.store(State::kIdle);
      return;
    }
  }
  if (state != State::kCapturing) return;

  auto [it, inserted] = g_threads.try_emplace(std::this_thread::get_id(), 0);
  if (inserted) it->second = uint16_t(g_threads.size() - 1);
  ++g_counts[index];
  ++g_call_sites[(uint64_t(index) << 32) | uint32_t(lr)];
  if (g_events.size() < kMaxEvents)
    g_events.push_back({index, uint32_t(lr), it->second, g_frame});
}

void TB2TraceHitAddress(uint32_t address, uint64_t lr) {
  if (g_state.load(std::memory_order_relaxed) == State::kIdle) return;
  const uint32_t index = FindIndex(address);
  if (index != UINT32_MAX) TB2TraceHit(index, lr);
}

void TB2TraceInit(const std::filesystem::path& out_dir) {
  g_out_dir = out_dir;
  g_marker_index = FindIndex(uint32_t(std::stoul(REXCVAR_GET(trace_frame_marker), nullptr, 16)));
  REXLOG_INFO("[trace] {} functions wrapped; F8 records {} frame(s) to {}", tb2_trace_count,
              REXCVAR_GET(trace_frames), out_dir.string());
  rex::ui::RegisterBind("bind_render_trace", "F8", "Record a render trace", Arm);
#if !defined(_WIN32)
  std::signal(SIGUSR2, OnSigUsr2);
#endif
  if (REXCVAR_GET(trace_at_start)) {
    std::lock_guard lock(g_mutex);
    BeginCapture();
    REXLOG_INFO("[trace] recording from launch");
  }
  // Signal polling, and the time limit: a trace whose frame marker never comes
  // (wrong marker, or no frames yet) is still written.
  std::thread([] {
    for (;;) {
#if !defined(_WIN32)
      if (g_signal.exchange(false)) Arm();
#endif
      if (g_state.load() == State::kCapturing) {
        std::lock_guard lock(g_mutex);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          g_capture_start).count();
        if (g_state.load() == State::kCapturing &&
            (secs >= REXCVAR_GET(trace_max_seconds) || g_events.size() >= kMaxEvents)) {
          REXLOG_INFO("[trace] stopping after {:.1f} s / {} calls ({} frame(s) seen)", secs,
                      g_events.size(), g_frame);
          WriteTrace();
          g_state.store(State::kIdle);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }).detach();
}
