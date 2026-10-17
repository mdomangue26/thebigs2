// thebigs2 - native renderer phase 1: shadow capture (docs/native-renderer.md).
//
// Hooks the XDK D3D draw functions, Clear, Resolve, Swap and the ring segment
// switch without changing what they do. While a capture runs (F6, or
// `kill -s RTMIN+1` on Linux), it records each frame as the native renderer
// would see it - render targets,
// shaders, textures and draw parameters read from the D3D device's shadow
// state - and checks that reading against the PM4 packets D3D writes to the
// GPU ring:
//
//   - every register / constant the draw's flush writes must equal the value
//     decoded from the shadow (validates the shadow layout, docs/d3d-map.md);
//   - each hooked draw must emit exactly one draw packet, and no draw packets
//     may appear outside the hooked draws, Clear and Resolve;
//   - a mirror of the GPU state rebuilt from all parsed packets must match the
//     shadow at every draw (nothing reaches the GPU behind the shadow's back).
//
// Writes <user data>/captures/native_capture_NNN.txt per frame.

#include "native_capture.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/keybinds.h>

#include "render_trace.h"

REXCVAR_DEFINE_INT32(native_capture_frames, 1, "The Bigs 2/Debug",
                     "Frames recorded per native-renderer shadow capture (F6)");

namespace {

// --- Guest memory ----------------------------------------------------------

struct Guest {
  uint8_t* base;
  const uint8_t* Ptr(uint32_t a) const {
#if defined(_WIN32)
    return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
#else
    return base + a;
#endif
  }
  uint32_t U32(uint32_t a) const {
    uint32_t v;
    std::memcpy(&v, Ptr(a), 4);
    return __builtin_bswap32(v);
  }
  uint64_t U64(uint32_t a) const {
    uint64_t v;
    std::memcpy(&v, Ptr(a), 8);
    return __builtin_bswap64(v);
  }
};

// --- XDK D3D device shadow layout (docs/d3d-map.md) --------------------------

constexpr uint32_t kRingWrite = 48;          // u32: last PM4 dword written (D3D pre-increments)
constexpr uint32_t kFetchConstants = 1152;   // 32 slots x 6 dwords (reg 0x4800..)
constexpr uint32_t kAluConstants = 1920;     // 512 float4 (reg 0x4000..): VS 0-255, PS 256-511
constexpr uint32_t kRenderTargets = 12448;   // D3DSurface* [4]
constexpr uint32_t kDepthStencil = 12464;    // D3DSurface*
constexpr uint32_t kTextures = 12552;        // D3DBaseTexture* per fetch slot
constexpr uint32_t kPixelShader = 12700;     // D3DPixelShader*
constexpr uint32_t kVertexShader = 12704;    // D3DVertexShader*
constexpr uint32_t kVertexDecl = 11824;      // ? (sub_823BCEE8)

struct Block {
  uint32_t reg, offset, count;
};
// Register blocks the draw flush writes from the shadow (one dirty bit each).
constexpr Block kBlocks[] = {
    {0x2000, 10368, 16}, {0x2100, 10444, 21}, {0x2180, 10528, 5},  {0x2200, 10548, 12},
    {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 8},
    {0x4900, 10112, 40},  // bool + loop constants (after the 512 ALU constants)
    {0x2388, 10272, 24},  // PA_CL_UCP_0..5 user clip planes (flushed by sub_823B7608)
};

// Shadow offset of a GPU register, or 0 if the shadow layout doesn't cover it.
uint32_t ShadowOffset(uint32_t reg) {
  if (reg == 0x2102) return 0;  // VGT_INDX_OFFSET: written by the draw from its arguments
  if (reg >= 0x4000 && reg < 0x4800) return kAluConstants + (reg - 0x4000) * 4;
  if (reg >= 0x4800 && reg < 0x4800 + 32 * 6) return kFetchConstants + (reg - 0x4800) * 4;
  for (const Block& b : kBlocks)
    if (reg >= b.reg && reg < b.reg + b.count) return b.offset + (reg - b.reg) * 4;
  return 0;
}

// Registers the capture prints per draw.
constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002,
                   RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201,
                   RB_COLORCONTROL = 0x2202, PA_SU_SC_MODE_CNTL = 0x2205,
                   RB_MODECONTROL = 0x2208, RB_COLOR_MASK = 0x2104;

// --- PM4 parsing ---------------------------------------------------------------

struct DrawPacket {
  uint32_t packet = 0;      // ring address
  uint32_t initiator = 0;   // VGT_DRAW_INITIATOR
  uint32_t index_base = 0;  // DRAW_INDX with DMA indices
  uint32_t index_size = 0;
  bool inline_indices = false;
};

// How a register value reached the ring.
enum class Via : uint8_t { kType0, kType1, kSetConstant, kLoadAluConstant };
constexpr const char* kViaNames[] = {"type0", "type1", "SET_CONSTANT", "LOAD_ALU_CONSTANT"};

struct RegWrite {
  uint32_t reg, value, packet;  // packet = ring address of the packet header
  Via via;
};

struct Parsed {
  std::vector<RegWrite> regs;  // in order
  std::vector<DrawPacket> draws;
  std::map<uint32_t, uint32_t> opcodes;  // type-3 opcode -> count
  std::vector<std::string> notes;        // unusual packets, for the event list
  bool overrun = false;
};

// SET_CONSTANT / LOAD_ALU_CONSTANT kinds: ALU, fetch, bool, loop, registers.
constexpr uint32_t kConstBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

void ParsePm4(const Guest& g, uint32_t p, uint32_t end, Parsed& out) {
  const uint32_t start = p;
  uint32_t recent[6] = {};  // last packet addresses, for desync reports
  uint32_t n_packets = 0;
  while (p < end) {
    recent[n_packets++ % 6] = p;
    const uint32_t h = g.U32(p);
    const uint32_t type = h >> 30;
    uint32_t size;  // dwords including the header
    if (type == 0) {
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      const uint32_t reg = h & 0x7FFF;
      const bool one_reg = h & 0x8000;
      size = count + 1;
      if (p + size * 4 > end) break;
      if (count >= 8 && out.notes.size() < 16) {
        std::string n = fmt::format("type0 at {:08X}: {} regs from {:04X}:", p, count, reg);
        for (uint32_t i = 0; i < count && i < 8; ++i) n += fmt::format(" {:08X}", g.U32(p + 4 + i * 4));
        out.notes.push_back(n);
      }
      for (uint32_t i = 0; i < count; ++i)
        out.regs.push_back({one_reg ? reg : reg + i, g.U32(p + 4 + i * 4), p, Via::kType0});
    } else if (type == 1) {
      size = 3;
      if (p + size * 4 > end) break;
      out.regs.push_back({h & 0x7FF, g.U32(p + 4), p, Via::kType1});
      out.regs.push_back({(h >> 11) & 0x7FF, g.U32(p + 8), p, Via::kType1});
    } else if (type == 2) {
      size = 1;
    } else {
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      const uint32_t op = (h >> 8) & 0x7F;
      size = count + 1;
      if (p + size * 4 > end) break;
      ++out.opcodes[op];
      if (op == 0x2D && count >= 1) {  // SET_CONSTANT
        const uint32_t d0 = g.U32(p + 4);
        const uint32_t kind = (d0 >> 16) & 0xFF, offset = d0 & 0x7FF;
        if (kind < 5)
          for (uint32_t i = 0; i + 1 < count; ++i)
            out.regs.push_back(
                {kConstBase[kind] + offset + i, g.U32(p + 8 + i * 4), p, Via::kSetConstant});
      } else if (op == 0x2F && count >= 3) {  // LOAD_ALU_CONSTANT: from (physical) memory
        const uint32_t address = g.U32(p + 4) & 0x3FFFFFFF;
        const uint32_t d1 = g.U32(p + 8);
        const uint32_t kind = (d1 >> 16) & 0xFF, offset = d1 & 0x7FF;
        const uint32_t size = g.U32(p + 12) & 0xFFF;
        if (kind < 5 && address < 0x20000000)
          for (uint32_t i = 0; i < size; ++i)
            out.regs.push_back({kConstBase[kind] + offset + i, g.U32(0xA0000000 + address + i * 4),
                                p, Via::kLoadAluConstant});
      } else if (op == 0x22 && count >= 2) {  // DRAW_INDX
        DrawPacket d;
        d.packet = p;
        d.initiator = g.U32(p + 8);
        if (count >= 4) {
          d.index_base = g.U32(p + 12);
          d.index_size = g.U32(p + 16);
        }
        out.draws.push_back(d);
      } else if (op == 0x36 && count >= 1) {  // DRAW_INDX_2
        DrawPacket d;
        d.packet = p;
        d.initiator = g.U32(p + 4);
        d.inline_indices = true;
        out.draws.push_back(d);
      }
    }
    p += size * 4;
  }
  if (p != end && p < end) {
    out.overrun = true;
    std::string n = fmt::format("parse stopped at {:08X} (region {:08X}..{:08X}), last packets:",
                                p, start, end);
    for (uint32_t i = 0; i < 6 && i < n_packets; ++i) {
      const uint32_t a = recent[(n_packets - 1 - i) % 6];
      n += fmt::format(" {:08X}={:08X}", a, g.U32(a));
    }
    n += "; dwords before the stop:";
    for (uint32_t a = std::max(start, p - 4 * 24); a < p + 8 && a < end; a += 4)
      n += fmt::format(" {:08X}", g.U32(a));
    out.notes.push_back(n);
  }
}

// --- Capture state ---------------------------------------------------------------

std::atomic<int> g_state{0};  // 0 idle, 1 armed (start at next swap), 2 capturing
std::atomic<uint32_t> g_device{0};  // the D3D device, remembered from the draws
std::recursive_mutex g_mutex;  // the segment hook runs inside the draw hooks
std::filesystem::path g_out_dir;
uint32_t g_serial = 0;
int g_frames_left = 0;

// Where the mirror's value of a register came from.
struct Source {
  uint32_t packet = 0;
  uint32_t draw = 0;  // draws done when it was written
  char region = '?';  // D = a draw's flush, B = between hooks, R = inside Resolve
  Via via = Via::kType0;
};

struct MirrorValue {
  uint32_t value;
  Source src;
};

struct Mismatch {
  uint32_t draw, reg, stream, shadow;
  Source src;
};

struct Frame {
  std::vector<std::string> events;
  uint32_t draws = 0, indexed = 0, resolves = 0, clears = 0;
  // validation
  uint64_t parsed_bytes = 0, regions = 0, discontinuities = 0, overruns = 0, segments = 0;
  uint32_t bad_draw_packets = 0;      // hooked draws whose region had != 1 draw packet
  uint32_t outside_draw_packets = 0;  // draw packets outside hooked draws, clears, resolves
  uint32_t clear_resolve_packets = 0;  // draw packets inside Clear / Resolve (expected)
  uint64_t writes_checked = 0, writes_unmapped = 0, writes_from_memory = 0;
  std::vector<Mismatch> write_mismatches;
  uint64_t state_checked = 0;
  std::vector<Mismatch> state_mismatches;
  std::map<uint32_t, uint32_t> unmapped_regs;  // reg -> writes
  std::map<uint32_t, uint32_t> opcodes;
  // render passes: consecutive draws with the same target registers
  struct Pass {
    uint32_t surface_info, color_info, depth_info, rt0, ds, draws;
  };
  std::vector<Pass> passes;
};

Frame g_frame;
uint32_t g_ring_last = 0;                            // end of the last parsed region
std::unordered_map<uint32_t, MirrorValue> g_mirror;  // GPU state rebuilt from packets
uint32_t g_frame_index = 0;

void Absorb(const Parsed& parsed, char region) {
  for (const RegWrite& w : parsed.regs)
    g_mirror[w.reg] = {w.value, {w.packet, g_frame.draws, region, w.via}};
  for (auto& [op, n] : parsed.opcodes) g_frame.opcodes[op] += n;
}

// Ring parsing. D3D writes packets contiguously within a ring segment and
// moves to the next segment in sub_823B2A50 (hooked below), so the region
// since the last hook is parsed up to each segment switch and continued from
// the new segment's start; the gap between segments is stale data.
Parsed g_pending;  // packets since the last commit (may span segments)

void Scan(const Guest& g, uint32_t now) {
  if (g_ring_last == 0 || now < g_ring_last || now - g_ring_last > (4u << 20)) {
    if (g_ring_last != 0 && now != g_ring_last) ++g_frame.discontinuities;
    g_ring_last = now;
    return;
  }
  if (now == g_ring_last) return;
  ParsePm4(g, g_ring_last, now, g_pending);
  g_frame.parsed_bytes += now - g_ring_last;
  g_ring_last = now;
}

Parsed TakePending() {
  Parsed p = std::move(g_pending);
  g_pending = Parsed{};
  ++g_frame.regions;
  if (p.overrun) ++g_frame.overruns;
  return p;
}

// Packets written outside the hooked draws: resolves' internals, swap, ...
void CatchUp(const Guest& g, uint32_t now, char region = 'B') {
  Scan(g, now);
  if (g_pending.regs.empty() && g_pending.draws.empty() && g_pending.notes.empty() &&
      g_pending.opcodes.empty())
    return;
  Parsed parsed = TakePending();
  if (region == 'C' || region == 'R') {
    g_frame.clear_resolve_packets += uint32_t(parsed.draws.size());
    Absorb(parsed, region);
    return;
  }
  g_frame.outside_draw_packets += uint32_t(parsed.draws.size());
  for (auto& d : parsed.draws)
    g_frame.events.push_back(fmt::format("X {} draw packet outside the hooks at {:08X}: prim {} n {} src {}",
                                         region, d.packet, d.initiator & 0x3F, d.initiator >> 16,
                                         (d.initiator >> 6) & 3));
  for (auto& n : parsed.notes) g_frame.events.push_back(fmt::format("N {} {}", region, n));
  Absorb(parsed, region);
}

// Next free ring address.
uint32_t RingPos(const Guest& g, uint32_t device) { return g.U32(device + kRingWrite) + 4; }

void WriteFrame() {
  std::error_code ec;
  std::filesystem::create_directories(g_out_dir, ec);
  const auto path = g_out_dir / fmt::format("native_capture_{:03}.txt", ++g_serial);
  std::ofstream out(path);
  const Frame& f = g_frame;
  out << fmt::format(
      "# The Bigs 2 native capture: frame {}, {} draws ({} indexed), {} clears, {} resolves\n",
      g_frame_index, f.draws, f.indexed, f.clears, f.resolves);
  out << "\n## validation\n";
  out << fmt::format(
      "ring: {} regions, {} bytes parsed, {} segment switches, {} discontinuities, {} overruns\n",
      f.regions, f.parsed_bytes, f.segments, f.discontinuities, f.overruns);
  out << fmt::format(
      "draw packets: {} hooked draws without exactly one, {} in clears/resolves, {} elsewhere\n",
      f.bad_draw_packets, f.clear_resolve_packets, f.outside_draw_packets);
  out << fmt::format(
      "flush writes vs shadow: {} checked, {} mismatches, {} to unmapped registers, {} constants "
      "loaded from memory (LOAD_ALU_CONSTANT, not checked)\n",
      f.writes_checked, f.write_mismatches.size(), f.writes_unmapped, f.writes_from_memory);
  for (size_t i = 0; i < f.write_mismatches.size() && i < 20; ++i) {
    auto& m = f.write_mismatches[i];
    out << fmt::format("  draw {} reg {:04X}: stream {:08X} shadow {:08X} ({})\n", m.draw, m.reg,
                       m.stream, m.shadow, kViaNames[int(m.src.via)]);
  }
  out << fmt::format("GPU state (mirror) vs shadow at draws: {} checked, {} mismatches\n",
                     f.state_checked, f.state_mismatches.size());
  std::map<uint32_t, uint32_t> by_reg;
  for (auto& m : f.state_mismatches) ++by_reg[m.reg];
  for (auto& [reg, n] : by_reg) {
    auto it = std::find_if(f.state_mismatches.begin(), f.state_mismatches.end(),
                           [&](auto& m) { return m.reg == reg; });
    out << fmt::format(
        "  reg {:04X}: {} draws, first draw {}: GPU {:08X} shadow {:08X}; GPU value written {} "
        "draw {} by {} at {:08X}\n",
        reg, n, it->draw, it->stream, it->shadow,
        it->src.region == 'D' ? "in the flush of" : it->src.region == 'R' ? "in a Resolve before"
                                                                         : "between hooks before",
        it->src.draw, kViaNames[int(it->src.via)], it->src.packet);
  }
  out << "registers written outside the shadow map (reg x writes):";
  for (auto& [reg, n] : f.unmapped_regs) out << fmt::format(" {:04X}x{}", reg, n);
  out << "\ntype-3 opcodes (op x count):";
  for (auto& [op, n] : f.opcodes) out << fmt::format(" {:02X}x{}", op, n);
  out << "\n\n## render passes (surface_info color_info depth_info rt0 ds draws)\n";
  for (auto& p : f.passes)
    out << fmt::format("{:08X} {:08X} {:08X} {:08X} {:08X} {}\n", p.surface_info, p.color_info,
                       p.depth_info, p.rt0, p.ds, p.draws);
  out << "\n## events\n";
  for (auto& e : f.events) out << e << '\n';
  REXLOG_INFO("[native] wrote {} ({} draws, {} flush mismatches, {} state mismatches)",
              path.string(), f.draws, f.write_mismatches.size(), f.state_mismatches.size());
}

void Arm() {
  int expected = 0;
  if (g_state.compare_exchange_strong(expected, 1))
    REXLOG_INFO("[native] capture armed: recording from the next frame");
}

#if !defined(_WIN32)
std::atomic<bool> g_signal{false};
void OnSignal(int) { g_signal.store(true); }
#endif

// The render target state a native renderer would read.
std::string Targets(const Guest& g, uint32_t device) {
  auto reg = [&](uint32_t r) { return g.U32(device + ShadowOffset(r)); };
  return fmt::format("rt {:08X} ds {:08X} si {:08X} ci {:08X} di {:08X}",
                     g.U32(device + kRenderTargets), g.U32(device + kDepthStencil),
                     reg(RB_SURFACE_INFO), reg(RB_COLOR_INFO), reg(RB_DEPTH_INFO));
}

// --- Hooks -------------------------------------------------------------------

struct DrawArgs {
  uint32_t device, r4, r5, r6, r7, r8;
  bool indexed;
};

DrawArgs BeginDraw(PPCContext& ctx, uint8_t* base, bool indexed) {
  Guest g{base};
  DrawArgs a{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, indexed};
  CatchUp(g, RingPos(g, a.device));
  return a;
}

void EndDraw(const DrawArgs& a, uint8_t* base) {
  Guest g{base};
  Frame& f = g_frame;
  const uint32_t n = f.draws++;
  if (a.indexed) ++f.indexed;
  Scan(g, RingPos(g, a.device));
  const Parsed parsed = TakePending();
  if (parsed.draws.size() != 1) ++f.bad_draw_packets;
  for (auto& note : parsed.notes) f.events.push_back(fmt::format("N D {}", note));

  // Flush writes against the shadow.
  for (const RegWrite& w : parsed.regs) {
    const uint32_t reg = w.reg, value = w.value;
    const uint32_t off = ShadowOffset(reg);
    if (!off) {
      ++f.writes_unmapped;
      ++f.unmapped_regs[reg];
      continue;
    }
    if (w.via == Via::kLoadAluConstant) {  // shader literals, from the shader object
      ++f.writes_from_memory;
      continue;
    }
    ++f.writes_checked;
    const uint32_t shadow = g.U32(a.device + off);
    if (shadow != value && f.write_mismatches.size() < 1000)
      f.write_mismatches.push_back({n, reg, value, shadow, {w.packet, n, 'D', w.via}});
  }
  // the flush's writes belong to this draw (g_frame.draws already counts it)
  for (const RegWrite& w : parsed.regs) g_mirror[w.reg] = {w.value, {w.packet, n, 'D', w.via}};
  for (auto& [op, cnt] : parsed.opcodes) f.opcodes[op] += cnt;

  // GPU state as rebuilt from the stream, against the shadow.
  for (auto& [reg, mv] : g_mirror) {
    const uint32_t off = ShadowOffset(reg);
    if (!off || mv.src.via == Via::kLoadAluConstant) continue;
    ++f.state_checked;
    const uint32_t shadow = g.U32(a.device + off);
    if (shadow != mv.value && f.state_mismatches.size() < 20000)
      f.state_mismatches.push_back({n, reg, mv.value, shadow, mv.src});
  }

  // What a native renderer would read for this draw.
  auto reg = [&](uint32_t r) { return g.U32(a.device + ShadowOffset(r)); };
  const uint32_t si = reg(RB_SURFACE_INFO), ci = reg(RB_COLOR_INFO), di = reg(RB_DEPTH_INFO);
  const uint32_t rt0 = g.U32(a.device + kRenderTargets), ds = g.U32(a.device + kDepthStencil);
  if (f.passes.empty() || f.passes.back().surface_info != si || f.passes.back().color_info != ci ||
      f.passes.back().depth_info != di || f.passes.back().rt0 != rt0 || f.passes.back().ds != ds)
    f.passes.push_back({si, ci, di, rt0, ds, 0});
  ++f.passes.back().draws;

  std::string tex;
  for (uint32_t s = 0; s < 32; ++s)
    if (uint32_t t = g.U32(a.device + kTextures + s * 4)) tex += fmt::format(" {}:{:08X}", s, t);
  const DrawPacket d = parsed.draws.empty() ? DrawPacket{} : parsed.draws.back();

  f.events.push_back(fmt::format(
      "D {} {} pk {} args {:08X} {:08X} {:08X} {:08X} {:08X} | prim {} n {} src {} ib {:08X}/{:X} | "
      "vs {:08X} ps {:08X} decl {:08X} | rt {:08X} ds {:08X} si {:08X} ci {:08X} di {:08X} | "
      "depth {:08X} blend {:08X} color {:08X} mode {:08X} cull {:08X} mask {:08X} | tex{}",
      n, a.indexed ? "idx" : "vtx", parsed.draws.size(), a.r4, a.r5, a.r6, a.r7, a.r8, d.initiator & 0x3F,
      d.initiator >> 16, (d.initiator >> 6) & 3, d.index_base, d.index_size,
      g.U32(a.device + kVertexShader), g.U32(a.device + kPixelShader),
      g.U32(a.device + kVertexDecl), rt0, ds, si, ci, di, reg(RB_DEPTHCONTROL),
      reg(RB_BLENDCONTROL0), reg(RB_COLORCONTROL), reg(RB_MODECONTROL), reg(PA_SU_SC_MODE_CNTL),
      reg(RB_COLOR_MASK), tex));
}

void OnSwapLocked(uint8_t* base) {
  Guest g{base};
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  const int state = g_state.load();
  if (state == 2) {
    if (device) CatchUp(g, RingPos(g, device));
    g_frame.events.push_back("S swap");
    WriteFrame();
    if (--g_frames_left <= 0) {
      g_state.store(0);
      return;
    }
  }
  if (state == 1 || state == 2) {
    if (state == 1) {
      g_frames_left = std::max(1, REXCVAR_GET(native_capture_frames));
      g_mirror.clear();
      g_pending = Parsed{};
      g_ring_last = 0;
    }
    g_frame = Frame{};
    g_pending = Parsed{};
    ++g_frame_index;
    g_ring_last = 0;  // set after the swap: it moves to a new ring segment itself
    g_state.store(2);
  }
}

}  // namespace

namespace tb2 {

void NativeCaptureInit(const std::filesystem::path& out_dir) {
  g_out_dir = out_dir;
  rex::ui::RegisterBind("bind_native_capture", "F6", "Record a native-renderer shadow capture",
                        Arm);
#if !defined(_WIN32)
  std::signal(SIGRTMIN + 1, OnSignal);
#endif
}

void NativeCaptureAfterSwap(uint8_t* base) {
  if (g_state.load(std::memory_order_relaxed) != 2) return;
  std::lock_guard lock(g_mutex);
  Guest g{base};
  if (const uint32_t device = g_device.load(std::memory_order_relaxed)) {
    g_pending = Parsed{};
    g_ring_last = RingPos(g, device);
  }
}

void NativeCaptureOnSwap(uint8_t* base) {
#if !defined(_WIN32)
  if (g_signal.exchange(false, std::memory_order_relaxed)) Arm();
#endif
  if (g_state.load(std::memory_order_relaxed) == 0) return;
  std::lock_guard lock(g_mutex);
  OnSwapLocked(base);
}

}  // namespace tb2

// sub_824DA698 DrawVertices / sub_824DAA88 DrawIndexedVertices (device in r3):
// flush the dirty shadow state into the ring, then write the draw packet.
extern "C" REX_FUNC(__imp__sub_824DA698);
extern "C" REX_FUNC(sub_824DA698) {
  TB2_TRACE_HOOKED(0x824DA698);
  g_device.store(ctx.r3.u32, std::memory_order_relaxed);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_824DA698(ctx, base);
  }
  std::lock_guard lock(g_mutex);
  const DrawArgs a = BeginDraw(ctx, base, false);
  __imp__sub_824DA698(ctx, base);
  EndDraw(a, base);
}

extern "C" REX_FUNC(__imp__sub_824DAA88);
extern "C" REX_FUNC(sub_824DAA88) {
  TB2_TRACE_HOOKED(0x824DAA88);
  g_device.store(ctx.r3.u32, std::memory_order_relaxed);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_824DAA88(ctx, base);
  }
  std::lock_guard lock(g_mutex);
  const DrawArgs a = BeginDraw(ctx, base, true);
  __imp__sub_824DAA88(ctx, base);
  EndDraw(a, base);
}

// sub_823C6450 Resolve(device, flags, src rect, dest texture, dest point, level,
// slice, clear color, [f1] clear z, ...): EDRAM -> texture copy, optional clear.
extern "C" REX_FUNC(__imp__sub_823C6450);
extern "C" REX_FUNC(sub_823C6450) {
  TB2_TRACE_HOOKED(0x823C6450);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_823C6450(ctx, base);
  }
  std::lock_guard lock(g_mutex);
  Guest g{base};
  const uint32_t device = ctx.r3.u32;
  CatchUp(g, RingPos(g, device));
  g_frame.events.push_back(fmt::format(
      "R {} flags {:08X} rect {:08X} dest {:08X} point {:08X} level {} slice {} clear {:08X} z {} "
      "| {}",
      g_frame.resolves++, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
      ctx.r10.u32, ctx.f1.f64, Targets(g, device)));
  __imp__sub_823C6450(ctx, base);
  CatchUp(g, RingPos(g, device), 'R');
}

// sub_823B2A50: the ring segment is full - kick it off to the GPU and continue
// in the next one (returns the new write pointer). Parse the old segment up to
// here; the new one starts at the returned pointer.
extern "C" REX_FUNC(__imp__sub_823B2A50);
extern "C" REX_FUNC(sub_823B2A50) {
  TB2_TRACE_HOOKED(0x823B2A50);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_823B2A50(ctx, base);
  }
  Guest g{base};
  const uint32_t device = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    Scan(g, RingPos(g, device));
  }
  __imp__sub_823B2A50(ctx, base);
  std::lock_guard lock(g_mutex);
  g_ring_last = RingPos(g, device);
  ++g_frame.segments;
}

// sub_823C5C28 Clear(device, count, rects, flags, color, [f1] z, stencil):
// clears the bound render targets with a rectangle draw of its own.
extern "C" REX_FUNC(__imp__sub_823C5C28);
extern "C" REX_FUNC(sub_823C5C28) {
  TB2_TRACE_HOOKED(0x823C5C28);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_823C5C28(ctx, base);
  }
  std::lock_guard lock(g_mutex);
  Guest g{base};
  const uint32_t device = ctx.r3.u32;
  CatchUp(g, RingPos(g, device));
  g_frame.events.push_back(fmt::format(
      "C {} count {} rects {:08X} flags {:08X} color {:08X} z {} stencil {} | {}", g_frame.clears++,
      ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r9.u32, Targets(g, device)));
  __imp__sub_823C5C28(ctx, base);
  CatchUp(g, RingPos(g, device), 'C');
}
