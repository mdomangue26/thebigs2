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
// Also logs BeginTiling/EndTiling (with every packet they write), and a
// resource table per frame: textures, vertex/index buffers and shader
// microcode the draws use, hashed and compared with the previous captured
// frame (native_capture_frames >= 2) to tell static from dynamic data.
//
// Phase 2 (resources): converts every texture the frame uses to its host
// format (src/native/textures.h; PNG previews with native_capture_textures),
// and tracks the vertex + pixel shader microcode of every draw and translates
// each pair to SPIR-V with the native renderer's shader library
// (src/native/shaders.h); tools/native_validate.sh checks the SPIR-V.
//
// Phase 3 (native_capture_render): also draws the captured frames with the
// native renderer (src/native/renderer.h) through Plume on the SDK's Vulkan
// device (the emulator keeps rendering; --vulkan_validation_enabled=true checks
// both), fed from
// the shadow at every draw, Clear and Resolve, and writes each resolve's image
// to native_capture_NNN_native/ for comparison with the emulator.
//
// Writes <user data>/captures/native_capture_NNN.txt per frame, and the
// translated shaders to native_capture_NNN_shaders/.

#include "native_capture.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <atomic>
#include <set>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/vulkan/provider.h>

#include "native/buffers.h"
#include "native/renderer.h"
#include "native/shaders.h"
#include "native/textures.h"
#include "render_trace.h"

REXCVAR_DEFINE_INT32(native_capture_frames, 1, "The Bigs 2/Debug",
                     "Frames recorded per native-renderer shadow capture (F6)");
REXCVAR_DEFINE_BOOL(native_capture_shaders, true, "The Bigs 2/Debug",
                    "Translate a native capture's shaders to SPIR-V and write them next to it");
REXCVAR_DEFINE_INT32(native_capture_textures, 0, "The Bigs 2/Debug",
                     "Write PNG previews of a native capture's converted textures, at most this "
                     "many pixels wide/high (0 = none)");
REXCVAR_DEFINE_BOOL(native_capture_render, false, "The Bigs 2/Debug",
                    "Also draw a native capture's frames with the native renderer (phase 3) and "
                    "write its resolves as PNGs next to the capture");

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
  uint16_t U16(uint32_t a) const {
    uint16_t v;
    std::memcpy(&v, Ptr(a), 2);
    return __builtin_bswap16(v);
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
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180, SQ_CONTEXT_MISC = 0x2181,
                   SQ_INTERPOLATOR_CNTL = 0x2182;
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

// Shader microcode load (IM_LOAD from memory, IM_LOAD_IMMEDIATE inline).
struct ShaderLoad {
  uint32_t type;     // 0 vertex, 1 pixel
  uint32_t address;  // physical, 0 for immediate loads
  uint32_t dwords;
  uint64_t hash;     // immediate loads: of the microcode
  uint32_t data;     // guest address of the microcode (in the ring for immediate loads)
};

struct Parsed {
  std::vector<RegWrite> regs;  // in order
  std::vector<ShaderLoad> shader_loads;
  std::vector<DrawPacket> draws;
  std::map<uint32_t, uint32_t> opcodes;  // type-3 opcode -> count
  std::vector<std::string> notes;        // unusual packets, for the event list
  bool overrun = false;
};

// SET_CONSTANT / LOAD_ALU_CONSTANT kinds: ALU, fetch, bool, loop, registers.
constexpr uint32_t kConstBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

bool g_log_packets = false;  // log every packet as a note (inside the tiling hooks)

void LogPacket(const Guest& g, uint32_t p, uint32_t size, Parsed& out) {
  if (out.notes.size() >= 400) return;
  std::string n = fmt::format("pkt {:08X}:", p);
  for (uint32_t i = 0; i < size && i < 12; ++i) n += fmt::format(" {:08X}", g.U32(p + i * 4));
  if (size > 12) n += fmt::format(" ... ({} dwords)", size);
  out.notes.push_back(n);
}

uint64_t Fnv(const uint8_t* data, size_t bytes) {
  uint64_t h = 0xCBF29CE484222325ull;
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, data + i, 8);
    h = (h ^ w) * 0x100000001B3ull;
  }
  for (; i < bytes; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

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
      } else if (op == 0x27 && count >= 2) {  // IM_LOAD
        const uint32_t addr_type = g.U32(p + 4), start_size = g.U32(p + 8);
        out.shader_loads.push_back({addr_type & 3, addr_type & ~3u, start_size & 0xFFFF, 0,
                                    0xA0000000 + (addr_type & ~3u)});
      } else if (op == 0x2B && count >= 2) {  // IM_LOAD_IMMEDIATE
        const uint32_t dwords = g.U32(p + 8) & 0xFFFF;
        if (dwords <= count - 2)
          out.shader_loads.push_back(
              {g.U32(p + 4) & 3, 0, dwords, Fnv(g.Ptr(p + 12), dwords * 4), p + 12});
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
    if (g_log_packets) {
      LogPacket(g, p, size, out);
    } else if (type == 3) {  // replays and predication: always worth seeing
      const uint32_t op = (h >> 8) & 0x7F;
      if (op == 0x3F || op == 0x37 || (op >= 0x60 && op <= 0x65)) LogPacket(g, p, size, out);
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
  // resources used by the frame's draws (see CollectResources)
  struct Texture {
    uint32_t fetch[6];
    uint32_t draws = 0;
    bool resolve_dest = false;
  };
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Texture> textures;  // base, dword1, dword2
  struct Buffer {
    uint32_t draws = 0;
  };
  std::map<std::tuple<char, uint32_t, uint32_t>, Buffer> buffers;  // kind I/V, address, bytes
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>, uint32_t> shaders;  // -> draws
  std::vector<uint32_t> resolve_dest_bases;
  // Shader pairs drawn with: vs, ps microcode hash, SQ_PROGRAM_CNTL,
  // SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL, RB_SURFACE_INFO, primitive -> draws.
  std::map<std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
           uint32_t>
      shader_pairs;
  uint32_t shader_unknown_draws = 0;  // microcode loaded before the capture, object not seen yet
  uint32_t shader_by_object = 0;      // draws whose shader came from the object map
  uint32_t shader_object_stale = 0;   // object map disagrees with the tracked microcode
  uint32_t ps_object_checked = 0, ps_object_mismatches = 0;
  // Geometry (phase 2, src/native/buffers.h): index conversion, and the highest
  // vertex each draw fetches against its vertex buffers' sizes.
  uint32_t geometry_converted = 0, geometry_failed = 0, geometry_generated_indices = 0;
  uint32_t vertex_fetches_checked = 0, vertex_fetches_out_of_range = 0;
  uint32_t vertex_fetches_computed = 0;  // computed fetch index: not checked
  std::vector<std::string> geometry_notes;
};

Frame g_frame;
uint32_t g_ring_last = 0;                            // end of the last parsed region
std::unordered_map<uint32_t, MirrorValue> g_mirror;  // GPU state rebuilt from packets
// Registers the shadow doesn't hold, as the packets last set them (the native
// renderer's register file takes them from here, e.g. the window scissor D3D
// sets for each render target).
std::map<uint32_t, uint32_t> g_ring_regs;
bool g_tiling = false;  // between BeginTiling and EndTiling
void NoteRingRegister(uint32_t reg, uint32_t value);
uint32_t g_frame_index = 0;

// --- Shaders ---------------------------------------------------------------
//
// D3D loads microcode only when the shader changes, so the current shaders are
// tracked across draws (and Clear/Resolve, which load their own). Draws early
// in a capture may use shaders loaded before it; those are found through the
// D3D objects they were loaded for (pixel shader object, vertex shader object +
// vertex declaration - D3D patches vertex shaders per declaration).

std::unordered_map<uint64_t, std::vector<uint32_t>> g_ucode;  // hash -> big-endian dwords
uint64_t g_current_shader[2] = {};                             // vertex, pixel (0 = unknown)
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_vs_by_object;  // (vs object, decl) -> hash
std::unordered_map<uint32_t, uint64_t> g_ps_by_object;

tb2::native::ShaderLibrary& Library() {
  static tb2::native::ShaderLibrary library;
  return library;
}

// Records the microcode of the loads; returns the hash per load.
std::vector<uint64_t> TrackShaderLoads(const Guest& g, const Parsed& parsed) {
  std::vector<uint64_t> hashes;
  for (const ShaderLoad& l : parsed.shader_loads) {
    if (l.type > 1 || !l.dwords || (l.address && l.address >= 0x20000000)) {
      hashes.push_back(0);
      continue;
    }
    const uint64_t h = Fnv(g.Ptr(l.data), l.dwords * 4) | 1;  // never 0
    auto& code = g_ucode[h];
    if (code.empty()) {
      code.resize(l.dwords);
      std::memcpy(code.data(), g.Ptr(l.data), l.dwords * 4);
    }
    g_current_shader[l.type] = h;
    hashes.push_back(h);
  }
  return hashes;
}

void Absorb(const Guest& g, const Parsed& parsed, char region) {
  TrackShaderLoads(g, parsed);
  for (const RegWrite& w : parsed.regs) {
    g_mirror[w.reg] = {w.value, {w.packet, g_frame.draws, region, w.via}};
    NoteRingRegister(w.reg, w.value);
  }
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
    Absorb(g, parsed, region);
    return;
  }
  g_frame.outside_draw_packets += uint32_t(parsed.draws.size());
  for (auto& d : parsed.draws)
    g_frame.events.push_back(fmt::format("X {} draw packet outside the hooks at {:08X}: prim {} n {} src {}",
                                         region, d.packet, d.initiator & 0x3F, d.initiator >> 16,
                                         (d.initiator >> 6) & 3));
  for (auto& n : parsed.notes) g_frame.events.push_back(fmt::format("N {} {}", region, n));
  Absorb(g, parsed, region);
}

// Next free ring address.
uint32_t RingPos(const Guest& g, uint32_t device) { return g.U32(device + kRingWrite) + 4; }

uint8_t* g_base = nullptr;  // guest memory, for hashing at the end of the frame
std::unordered_map<std::string, uint64_t> g_prev_hashes;  // resource key -> hash, this capture

// Hash of a resource's memory (physical address) and its status against the
// previous frames of this capture: new / same / changed.
char HashStatus(const Guest& g, const std::string& key, uint32_t phys, uint32_t bytes,
                uint64_t* hash_out) {
  if (phys >= 0x20000000 || bytes == 0) return '?';
  const uint64_t h = Fnv(g.Ptr(0xA0000000 + phys), std::min<uint32_t>(bytes, 32u << 20));
  *hash_out = h;
  auto [it, inserted] = g_prev_hashes.try_emplace(key, h);
  if (inserted) return 'n';
  const char status = it->second == h ? 's' : 'c';
  it->second = h;
  return status;
}

// Minimal PNG writer (stored deflate blocks) for the texture previews;
// tools/native_contact_sheet.py recompresses them into one sheet.
void WritePng(const std::filesystem::path& path, uint32_t w, uint32_t h,
              const std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(w * 4 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t i = 0; i < raw.size() || i == 0; i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n >= raw.size() ? 1 : 0);
    z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)});
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    if (raw.empty()) break;
  }
  const uint32_t adler = b << 16 | a;
  z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});
  std::ofstream f(path, std::ios::binary);
  auto be32 = [&](std::vector<uint8_t>& v, uint32_t x) {
    v.insert(v.end(), {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)});
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> c;
    be32(c, uint32_t(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 4; i < c.size(); ++i) {
      crc ^= c[i];
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    be32(c, ~crc);
    f.write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr;
  be32(ihdr, w);
  be32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
}

void WriteResources(std::ofstream& out, const Guest& g, const std::filesystem::path& preview_dir) {
  namespace xg = rex::graphics;
  const Frame& f = g_frame;
  std::map<char, std::map<char, uint32_t>> summary;  // kind -> status -> count
  std::vector<std::string> lines;
  uint32_t converted_count[2] = {};
  std::vector<std::string> conversion_errors;
  const int preview_edge = REXCVAR_GET(native_capture_textures);
  if (preview_edge > 0) {
    std::error_code ec;
    std::filesystem::create_directories(preview_dir, ec);
  }
  for (auto& [key, t] : f.textures) {
    xg::xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, t.fetch, sizeof(t.fetch));
    uint32_t w = 1, h = 1, d = 1;
    switch (fetch.dimension) {
      case xg::xenos::DataDimension::k1D: w = fetch.size_1d.width + 1; break;
      case xg::xenos::DataDimension::k2DOrStacked:
        w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1;
        d = fetch.stacked ? fetch.size_2d.stack_depth + 1 : 1;
        break;
      case xg::xenos::DataDimension::k3D:
        w = fetch.size_3d.width + 1, h = fetch.size_3d.height + 1, d = fetch.size_3d.depth + 1;
        break;
      case xg::xenos::DataDimension::kCube:
        w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1, d = 6;
        break;
    }
    const auto* info = xg::FormatInfo::Get(uint32_t(fetch.format));
    const auto layout = xg::texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, w, h, d, fetch.tiled, fetch.format, fetch.packed_mips, true,
        fetch.mip_max_level);
    const uint32_t base = fetch.base_address << 12, bytes = layout.base.level_data_extent_bytes;
    const bool rt = std::find(f.resolve_dest_bases.begin(), f.resolve_dest_bases.end(),
                              fetch.base_address) != f.resolve_dest_bases.end();
    uint64_t hash = 0;
    const char st = HashStatus(g, fmt::format("t{:08X}{:08X}{:08X}", base, t.fetch[1], t.fetch[2]),
                               base, bytes, &hash);
    ++summary[rt ? 'R' : 'T'][st];
    // Phase 2: convert to the host format (and preview).
    tb2::native::HostTexture host;
    std::string error;
    const bool converted = tb2::native::ConvertTexture(g.base, fetch, host, &error);
    ++converted_count[converted];
    if (!converted && conversion_errors.size() < 20)
      conversion_errors.push_back(fmt::format("{:08X} {}: {}", base, info ? info->name : "?", error));
    if (converted && preview_edge > 0) {
      uint32_t level = 0;
      while (level + 1 < host.levels.size() &&
             std::max(host.levels[level].width, host.levels[level].height) > uint32_t(preview_edge))
        ++level;
      std::vector<uint8_t> rgba;
      if (tb2::native::DecodeToRgba8(host, level, 0, rgba))
        WritePng(preview_dir / fmt::format("{}_{:08X}_{}_{}x{}.png", rt ? "rt" : "tex", base,
                                           info ? info->name : "?", w, h),
                 host.levels[level].width, host.levels[level].height, rgba);
    }
    lines.push_back(fmt::format(
        "{} {:08X} {:<16} {}x{}x{} dim {} pitch {} {} mips {}-{}{} mipaddr {:08X} | {} bytes, {} "
        "draws, {} {:016X}",
        rt ? "rtex" : "tex ", base, info ? info->name : "?", w, h, d, uint32_t(fetch.dimension),
        fetch.pitch * 32, fetch.tiled ? "tiled" : "linear", uint32_t(fetch.mip_min_level),
        uint32_t(fetch.mip_max_level), fetch.packed_mips ? " packed" : "",
        fetch.mip_address << 12, bytes, t.draws, st, hash));
  }
  for (auto& [key, b] : f.buffers) {
    auto [kind, address, bytes] = key;
    uint64_t hash = 0;
    const char st = HashStatus(g, fmt::format("{}{:08X}{:08X}", kind, address, bytes), address,
                               bytes, &hash);
    ++summary[kind][st];
    lines.push_back(fmt::format("{}  {:08X} {} bytes, {} draws, {} {:016X}",
                                kind == 'I' ? "ib" : "vb", address, bytes, b.draws, st, hash));
  }
  for (auto& [key, draws] : f.shaders) {
    auto [type, address, dwords, imm_hash] = key;
    uint64_t hash = imm_hash;
    char st = 'i';  // immediate: microcode was in the ring
    if (address) st = HashStatus(g, fmt::format("s{:08X}", address), address, dwords * 4, &hash);
    ++summary['S'][st];
    lines.push_back(fmt::format("{} {:08X} {} dwords, {} loads, {} {:016X}",
                                type ? "ps " : "vs ", address, dwords, draws, st, hash));
  }
  out << "\n## resources (status: n = first seen in this capture, s = same as the previous "
         "frame, c = changed, i = immediate)\n";
  static constexpr std::pair<char, const char*> kKinds[] = {
      {'T', "textures"}, {'R', "resolve targets"}, {'V', "vertex buffers"},
      {'I', "index buffers"}, {'S', "shader loads"}};
  for (auto [kind, name] : kKinds) {
    out << fmt::format("{}:", name);
    for (auto [st, n] : summary[kind]) out << fmt::format(" {}={}", st, n);
    out << '\n';
  }
  out << fmt::format("texture conversion (phase 2): {} converted, {} failed\n", converted_count[1],
                     converted_count[0]);
  for (auto& e : conversion_errors) out << "  " << e << '\n';
  for (auto& l : lines) out << l << '\n';
}

// Translates every shader pair the frame drew with, as the native renderer
// would, and writes the SPIR-V (plus the microcode disassembly) to dir.
void WriteShaders(std::ofstream& out, const std::filesystem::path& dir) {
  namespace xg = rex::graphics;
  tb2::native::ShaderLibrary& library = Library();
  const Frame& f = g_frame;
  const bool write = REXCVAR_GET(native_capture_shaders);
  std::error_code ec;
  if (write) std::filesystem::create_directories(dir, ec);
  std::map<std::pair<const void*, uint64_t>, bool> translations;  // (shader, modification) -> ok
  std::map<const void*, bool> disassembled;
  uint32_t failed_pairs = 0;
  std::vector<std::string> lines;
  auto emit = [&](xg::SpirvShader& shader, xg::SpirvShaderTranslator::Modification mod,
                  const char* kind) {
    auto [it, inserted] = translations.try_emplace({&shader, mod.value}, false);
    if (!inserted) return it->second;
    const auto* t = library.Translate(shader, mod);
    it->second = t != nullptr;
    if (!write) return it->second;
    const auto base = fmt::format("{}_{:016X}", kind, shader.ucode_data_hash());
    if (!disassembled[&shader]) {
      disassembled[&shader] = true;
      std::ofstream(dir / (base + ".ucode.txt")) << shader.ucode_disassembly();
    }
    if (t) {
      const auto& bin = t->translated_binary();
      std::ofstream(dir / fmt::format("{}_{:016X}.spv", base, mod.value), std::ios::binary)
          .write(reinterpret_cast<const char*>(bin.data()), std::streamsize(bin.size()));
    }
    return it->second;
  };
  for (auto& [key, draws] : f.shader_pairs) {
    auto [vs_hash, ps_hash, program_cntl, context_misc, interpolator_cntl, surface_info, prim] = key;
    const auto& vs_code = g_ucode[vs_hash];
    const auto& ps_code = g_ucode[ps_hash];
    auto* vs = library.Get(xg::xenos::ShaderType::kVertex, vs_code);
    auto* ps = library.Get(xg::xenos::ShaderType::kPixel, ps_code);
    tb2::native::ShaderPairState state;
    state.sq_program_cntl.value = program_cntl;
    state.sq_context_misc.value = context_misc;
    state.sq_interpolator_cntl.value = interpolator_cntl;
    xg::reg::RB_SURFACE_INFO si;
    si.value = surface_info;
    state.msaa_samples = si.msaa_samples;
    state.primitive_type = xg::xenos::PrimitiveType(prim);
    const auto mods = library.Modifications(*vs, ps, state);
    const bool vs_ok = emit(*vs, mods.vertex, "vs");
    const bool ps_ok = emit(*ps, mods.pixel, "ps");
    if (!vs_ok || !ps_ok) ++failed_pairs;
    lines.push_back(fmt::format(
        "vs {:016X}/{:016X} ({} dwords) ps {:016X}/{:016X} ({} dwords) prim {} | {} draws | {}{} "
        "| textures {} interpolators {:04X}",
        vs->ucode_data_hash(), mods.vertex.value, vs_code.size(), ps->ucode_data_hash(),
        mods.pixel.value, ps_code.size(), prim, draws, vs_ok ? "" : "VS FAILED ",
        ps_ok ? "ok" : "PS FAILED", ps->texture_bindings().size(),
        mods.vertex.vertex.interpolator_mask));
  }
  uint32_t failed = 0;
  for (auto& [k, ok] : translations) failed += !ok;
  out << fmt::format(
      "\n## shaders (phase 2): {} pairs, {} translations ({} failed), {} pairs failed\n"
      "draws: {} with unknown shaders, {} found through the D3D objects, {} object map stale; "
      "pixel shader objects: {} checked, {} microcode address mismatches\n",
      f.shader_pairs.size(), translations.size(), failed, failed_pairs, f.shader_unknown_draws,
      f.shader_by_object, f.shader_object_stale, f.ps_object_checked, f.ps_object_mismatches);
  if (write) out << fmt::format("written to {}\n", dir.string());
  out << fmt::format(
      "geometry: {} draws converted ({} with generated indices), {} failed; vertex fetches: {} "
      "checked, {} beyond their buffer, {} with a computed index (not checked)\n",
      f.geometry_converted, f.geometry_generated_indices, f.geometry_failed,
      f.vertex_fetches_checked, f.vertex_fetches_out_of_range, f.vertex_fetches_computed);
  for (auto& n : f.geometry_notes) out << "  " << n << '\n';
  for (auto& l : lines) out << l << '\n';
}

void WriteNative(std::ofstream& out, const std::filesystem::path& dir);

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
  WriteResources(out, Guest{g_base},
                 g_out_dir / fmt::format("native_capture_{:03}_textures", g_serial));
  WriteShaders(out, g_out_dir / fmt::format("native_capture_{:03}_shaders", g_serial));
  WriteNative(out, g_out_dir / fmt::format("native_capture_{:03}_native", g_serial));
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

// D3D objects hold CPU (virtual) addresses; the GPU sees physical ones. The
// 0xE0000000 view of physical memory is offset by 4 KB (as SetTexture does).
uint32_t GpuAddress(uint32_t va) { return (va & 0x1FFFFFFF) + (va >= 0xE0000000 ? 0x1000 : 0); }

// Resources a draw uses: the fetch constants its flush wrote (so they're live
// for this draw), its index buffer and its shader microcode loads.
void CollectResources(const Guest& g, uint32_t device, const Parsed& parsed) {
  Frame& f = g_frame;
  uint32_t slots = 0;  // fetch constant slots written
  for (const RegWrite& w : parsed.regs)
    if (w.reg >= 0x4800 && w.reg < 0x4800 + 32 * 6) slots |= 1u << ((w.reg - 0x4800) / 6);
  for (uint32_t s = 0; s < 32; ++s) {
    if (!(slots & (1u << s))) continue;
    const uint32_t at = device + kFetchConstants + s * 24;
    uint32_t fc[6];
    for (int i = 0; i < 6; ++i) fc[i] = g.U32(at + i * 4);
    const uint32_t type = fc[0] & 3;
    if (type == 2) {  // texture
      auto& t = f.textures[{fc[1] >> 12, fc[1], fc[2]}];
      std::memcpy(t.fetch, fc, sizeof(fc));
      ++t.draws;
    } else if (type == 3) {  // three vertex fetches
      for (int v = 0; v < 3; ++v) {
        if ((fc[v * 2] & 3) != 3) continue;
        const uint32_t address = fc[v * 2] & ~3u, bytes = ((fc[v * 2 + 1] >> 2) & 0xFFFFFF) * 4;
        if (bytes) ++f.buffers[{'V', address, bytes}].draws;
      }
    }
  }
  for (const DrawPacket& d : parsed.draws) {
    if (d.inline_indices || !d.index_base) continue;
    const uint32_t count = d.initiator >> 16, size = (d.initiator >> 11) & 1 ? 4 : 2;
    ++f.buffers[{'I', d.index_base, count * size}].draws;
  }
  for (const ShaderLoad& l : parsed.shader_loads) ++f.shaders[{l.type, l.address, l.dwords, l.hash}];
}

uint32_t g_index_offset = 0;  // VGT_INDX_OFFSET, set by the draws from their arguments

// Index registers (register * 4 + component) a vertex shader's fetches use
// after an instruction wrote them, e.g. the instancing in the crowd shaders:
// raw vertex indices don't bound those fetches. From the microcode
// disassembly, cached per shader.
const std::set<uint32_t>& ComputedFetchIndices(const rex::graphics::Shader& shader) {
  static std::unordered_map<const void*, std::set<uint32_t>> cache;
  auto [it, inserted] = cache.try_emplace(&shader);
  if (!inserted) return it->second;
  // "op rN.mask, ..." (destination) and "vfetch_full rD, rN.c, vfK" (index)
  static const std::regex kInstr(R"(([a-z_][a-z_0-9]*)\s+r(\d+)(\.([xyzw_]{4}))?)");
  static const std::regex kFetch(R"(vfetch_full\s+\S+,\s+r(\d+)\.([xyzw]))");
  std::map<uint32_t, uint32_t> written;  // register -> component mask written so far
  std::istringstream lines(shader.ucode_disassembly());
  std::string line;
  while (std::getline(lines, line)) {
    std::smatch m;
    if (std::regex_search(line, m, kFetch)) {
      const uint32_t reg = uint32_t(std::stoul(m[1])), c = uint32_t(std::string("xyzw").find(m[2].str()[0]));
      if (written[reg] & (1u << c)) it->second.insert(reg * 4 + c);
    }
    if (std::regex_search(line, m, kInstr)) {
      const std::string mask = m[4].matched ? m[4].str() : "xyzw";
      for (uint32_t c = 0; c < 4; ++c)
        if (mask[c] != '_') written[uint32_t(std::stoul(m[2]))] |= 1u << c;
    }
  }
  return it->second;
}

// The draw's geometry as the native renderer would convert it, and whether the
// vertices it can reach lie inside the vertex buffers its vertex shader fetches
// from (vertex bindings: fetch constant + stride, from the microcode).
void CheckGeometry(const Guest& g, uint32_t device, const Parsed& parsed) {
  namespace xg = rex::graphics;
  Frame& f = g_frame;
  for (const RegWrite& w : parsed.regs)
    if (w.reg == 0x2102) g_index_offset = w.value;
  if (parsed.draws.size() != 1) return;
  const DrawPacket& d = parsed.draws.back();
  tb2::native::HostDraw host;
  std::string error;
  if (!tb2::native::ConvertDraw(g.base, {d.initiator, d.index_base, d.index_size}, host, &error)) {
    ++f.geometry_failed;
    if (f.geometry_notes.size() < 20) f.geometry_notes.push_back(error);
    return;
  }
  ++f.geometry_converted;
  const bool dma = ((d.initiator >> 6) & 3) == 0;
  if (host.indexed && !dma) ++f.geometry_generated_indices;
  if (!g_current_shader[0]) return;
  auto* vs = Library().Get(xg::xenos::ShaderType::kVertex, g_ucode[g_current_shader[0]]);
  const uint32_t max_index = host.indexed ? host.max_index : host.count ? host.count - 1 : 0;
  const uint32_t max_vertex = (max_index + g_index_offset) & 0xFFFFFF;
  const auto& computed = ComputedFetchIndices(*vs);
  for (const auto& binding : vs->vertex_bindings()) {
    bool computed_index = false;
    for (const auto& a : binding.attributes) {
      const auto& index = a.fetch_instr.operands[0];
      if (!a.fetch_instr.is_mini_fetch &&
          computed.count(index.storage_index * 4 + uint32_t(index.components[0])))
        computed_index = true;
    }
    if (computed_index) {
      ++f.vertex_fetches_computed;
      continue;
    }
    const uint32_t at = device + kFetchConstants + binding.fetch_constant * 8;
    const uint32_t dword1 = g.U32(at + 4);
    const uint32_t size_words = (dword1 >> 2) & 0xFFFFFF;
    uint32_t needed = 0;  // words of the last vertex
    std::string attributes;
    for (const auto& a : binding.attributes) {
      const auto& at = a.fetch_instr.attributes;
      const uint32_t used = a.fetch_instr.result.GetUsedResultComponents();
      // a mask of the words read
      const uint32_t words =
          32 - uint32_t(std::countl_zero(xg::xenos::GetVertexFormatNeededWords(at.data_format, used)));
      needed = std::max(needed, uint32_t(at.offset) + words);
      attributes += fmt::format(" [offset {} format {} used {:X} words {}]", at.offset,
                                uint32_t(at.data_format), used, words);
    }
    ++f.vertex_fetches_checked;
    const uint64_t end = uint64_t(max_vertex) * binding.stride_words + needed;
    if (end > size_words) {
      ++f.vertex_fetches_out_of_range;
      if (f.geometry_notes.size() < 20)
        f.geometry_notes.push_back(fmt::format(
            "draw {}: vertex {} (indices up to {} + offset {}) of fetch constant {} needs {} words, "
            "buffer {:08X} has {} (stride {})",
            f.draws - 1, max_vertex, max_index, g_index_offset, binding.fetch_constant, end,
            g.U32(at) & ~3u, size_words, binding.stride_words) + attributes);
    }
  }
}

// The shaders a draw uses (see TrackShaderLoads), and the translation state.
void TrackDrawShaders(const Guest& g, uint32_t device, const Parsed& parsed, uint32_t prim) {
  Frame& f = g_frame;
  const std::vector<uint64_t> loaded = TrackShaderLoads(g, parsed);
  const uint32_t vs_object = g.U32(device + kVertexShader), decl = g.U32(device + kVertexDecl);
  const uint32_t ps_object = g.U32(device + kPixelShader);
  bool loaded_type[2] = {};
  for (size_t i = 0; i < loaded.size(); ++i) {
    const ShaderLoad& l = parsed.shader_loads[i];
    if (!loaded[i]) continue;
    loaded_type[l.type] = true;
    if (l.type == 0) {
      g_vs_by_object[{vs_object, decl}] = loaded[i];
    } else {
      g_ps_by_object[ps_object] = loaded[i];
      // D3DPixelShader: microcode at *(ps + 24) + header[+40], header at ps +
      // *(ps + 64), size in bytes at header[+44] (from the flush, sub_823B8708).
      if (ps_object && l.address) {
        const uint32_t header = ps_object + g.U32(ps_object + 64);
        const uint32_t address = GpuAddress(g.U32(ps_object + 24) + g.U32(header + 40));
        const uint32_t dwords = g.U32(header + 44) / 4;
        ++f.ps_object_checked;
        if (address != l.address || dwords != l.dwords) {
          if (f.ps_object_mismatches++ < 10)
            f.events.push_back(fmt::format(
                "N D pixel shader object {:08X}: microcode {:08X}/{} from the object, {:08X}/{} "
                "loaded", ps_object, address, dwords, l.address, l.dwords));
        }
      }
    }
  }
  auto from_object = [&](int type, uint64_t found) {
    if (loaded_type[type] || !found) return;
    uint64_t& current = g_current_shader[type];
    if (!current) {
      current = found;
      ++f.shader_by_object;
    } else if (current != found) {
      ++f.shader_object_stale;
    }
  };
  auto vs_it = g_vs_by_object.find({vs_object, decl});
  from_object(0, vs_it != g_vs_by_object.end() ? vs_it->second : 0);
  auto ps_it = g_ps_by_object.find(ps_object);
  from_object(1, ps_it != g_ps_by_object.end() ? ps_it->second : 0);
  if (!g_current_shader[0] || !g_current_shader[1]) {
    ++f.shader_unknown_draws;
    return;
  }
  auto reg = [&](uint32_t r) { return g.U32(device + ShadowOffset(r)); };
  ++f.shader_pairs[{g_current_shader[0], g_current_shader[1], reg(SQ_PROGRAM_CNTL),
                    reg(SQ_CONTEXT_MISC), reg(SQ_INTERPOLATOR_CNTL), reg(RB_SURFACE_INFO), prim}];
}

// The render target state a native renderer would read.
std::string Targets(const Guest& g, uint32_t device) {
  auto reg = [&](uint32_t r) { return g.U32(device + ShadowOffset(r)); };
  return fmt::format("rt {:08X} ds {:08X} si {:08X} ci {:08X} di {:08X}",
                     g.U32(device + kRenderTargets), g.U32(device + kDepthStencil),
                     reg(RB_SURFACE_INFO), reg(RB_COLOR_INFO), reg(RB_DEPTH_INFO));
}

// --- Native rendering (phase 3) -------------------------------------------------
//
// With native_capture_render, every captured frame is also drawn by the native
// renderer: the register file below is rebuilt from the shadow at each draw,
// Clear and Resolve (registers the shadow doesn't hold keep the values the
// emulator's register file had at the start of the frame - listed in the
// capture file), and the draw's shaders come from the tracked microcode.

std::unique_ptr<tb2::native::Renderer> g_renderer;
bool g_renderer_failed = false;
bool g_render_frame = false;  // this captured frame is being drawn natively
std::unique_ptr<rex::graphics::RegisterFile> g_regs;
std::vector<tb2::native::ResolvedImage> g_native_resolves;
std::string g_native_status;
tb2::native::RendererStats g_native_stats;
uint32_t g_native_unknown_shaders = 0, g_native_literal_entries = 0;
std::vector<std::string> g_native_seeded;  // nonzero registers outside the shadow

// Literal constants D3D loads with a shader (sub_823B8330, called from the
// shader flush sub_823B8708): a table at header + *(header + 20), *(table + 16)
// bytes of entries from table + 20: {u16 vec4 index, u16 dword count, u32 data
// offset from base}. Pixel shaders: header = object + 40, base = *(object +
// 24); vertex shaders: header = object + 872, base = *(object + 32).
void OverlayLiterals(const Guest& g, uint32_t header, uint32_t base,
                     rex::graphics::RegisterFile& regs) {
  const uint32_t offset = g.U32(header + 20);
  if (!offset) return;
  const uint32_t table = header + offset;
  const uint32_t end = table + 20 + g.U32(table + 16);
  for (uint32_t entry = table + 20, guard = 0; entry < end && guard < 512; entry += 8, ++guard) {
    const uint32_t vec4 = g.U16(entry), dwords = g.U16(entry + 2);
    if (!dwords) break;
    const uint32_t data = base + g.U32(entry + 4);
    for (uint32_t i = 0; i < dwords && vec4 * 4 + i < 2048; ++i)
      regs[0x4000 + vec4 * 4 + i] = g.U32(data + i * 4);
    ++g_native_literal_entries;
  }
}

bool InShadow(uint32_t reg) { return reg == 0x2102 || ShadowOffset(reg) != 0; }

void NoteRingRegister(uint32_t reg, uint32_t value) {
  if (reg >= 0x2000 && reg < 0x4000 && !InShadow(reg)) g_ring_regs[reg] = value;
}

// The GPU state as the native renderer reads it at a draw / Clear / Resolve.
void FillRegisters(const Guest& g, uint32_t device) {
  auto& regs = *g_regs;
  auto copy = [&](uint32_t reg, uint32_t offset, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) regs[reg + i] = g.U32(device + offset + i * 4);
  };
  for (const Block& b : kBlocks) copy(b.reg, b.offset, b.count);
  copy(0x4000, kAluConstants, 2048);
  copy(0x4800, kFetchConstants, 32 * 6);
  regs[0x2102] = g_index_offset;
  for (auto [reg, value] : g_ring_regs) regs[reg] = value;
  if (g_tiling) {
    // Predicated tiling: the GPU replays the scene per tile with a tile-sized
    // window scissor; the native renderer draws it once, at full size.
    regs[0x2080] = 0;           // PA_SC_WINDOW_OFFSET
    regs[0x2081] = 0x80000000;  // PA_SC_WINDOW_SCISSOR_TL (window offset disabled)
    regs[0x2082] = 0x20002000;  // PA_SC_WINDOW_SCISSOR_BR: 8192x8192
  }
  if (uint32_t vs = g.U32(device + kVertexShader))
    OverlayLiterals(g, vs + 872, g.U32(vs + 32), regs);
  if (uint32_t ps = g.U32(device + kPixelShader)) OverlayLiterals(g, ps + 40, g.U32(ps + 24), regs);
}

// Surfaces D3D has set: bits 0-3 render targets, bit 4 depth / stencil.
uint32_t BoundSurfaces(const Guest& g, uint32_t device) {
  uint32_t bound = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (g.U32(device + kRenderTargets + i * 4)) bound |= 1u << i;
  if (g.U32(device + kDepthStencil)) bound |= 0x10;
  return bound;
}

void BeginNativeFrame(uint8_t* base) {
  g_render_frame = false;
  g_tiling = false;  // tiling never spans a Swap
  g_native_resolves.clear();
  g_native_unknown_shaders = g_native_literal_entries = 0;
  if (!REXCVAR_GET(native_capture_render) || g_renderer_failed) return;
  if (!g_renderer) {
    std::string error = "the GPU backend isn't Vulkan";
    auto* graphics = rex::system::kernel_state()->emulator()->graphics_system();
    auto* provider =
        graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
    if (provider) g_renderer = tb2::native::Renderer::Create(provider->vulkan_device(), &error);
    if (!g_renderer) {
      g_renderer_failed = true;
      g_native_status = "native renderer unavailable: " + error;
      REXLOG_WARN("[native] {}", g_native_status);
      return;
    }
    g_native_status = "native renderer on " + g_renderer->DeviceName();
  }
  if (!g_regs) g_regs = std::make_unique<rex::graphics::RegisterFile>();
  // Seed with the emulator's GPU registers for what the shadow doesn't hold.
  g_native_seeded.clear();
  auto* graphics = static_cast<rex::graphics::GraphicsSystem*>(
      rex::system::kernel_state()->emulator()->graphics_system());
  if (graphics) {
    std::memcpy(g_regs->values, graphics->register_file()->values, sizeof(g_regs->values));
    for (uint32_t r = 0x2000; r < 0x2400; ++r)
      if (g_regs->values[r] && !InShadow(r) && g_native_seeded.size() < 64)
        g_native_seeded.push_back(fmt::format("{:04X}={:08X}", r, g_regs->values[r]));
  }
  g_renderer->BeginFrame(base);
  g_render_frame = true;
}

void NativeDraw(const Guest& g, uint32_t device, const Parsed& parsed) {
  if (!g_render_frame || parsed.draws.size() != 1) return;
  if (!g_current_shader[0] || !g_current_shader[1]) {
    ++g_native_unknown_shaders;
    return;
  }
  const DrawPacket& d = parsed.draws.back();
  FillRegisters(g, device);
  auto& regs = *g_regs;
  regs[0x21FC] = d.initiator;  // VGT_DRAW_INITIATOR
  regs[0x21FA] = d.index_base;  // VGT_DMA_BASE
  regs[0x21FB] = d.index_size;  // VGT_DMA_SIZE
  g_renderer->Draw(regs, g_ucode[g_current_shader[0]], g_ucode[g_current_shader[1]],
                   {d.initiator, d.index_base, d.index_size});
}

// The emulator's results of the frame's resolves, for comparison: with the
// SDK's readback_resolve=full the emulator writes each resolve back to guest
// memory, where the destination textures can be read once the GPU has finished
// the frame - shortly after its Swap (only the last resolve to each texture
// survives).
struct EmulatorResolve {
  uint32_t index;
  uint32_t fetch[6];
};
std::vector<EmulatorResolve> g_frame_resolves;  // this frame's
std::vector<EmulatorResolve> g_emu_pending;     // the previous captured frame's
std::filesystem::path g_emu_pending_dir;

void WriteEmulatorResolves(uint8_t* base) {
  std::map<uint32_t, const EmulatorResolve*> last;  // destination -> last resolve to it
  for (const auto& r : g_emu_pending) last[r.fetch[1] >> 12] = &r;
  std::error_code ec;
  std::filesystem::create_directories(g_emu_pending_dir, ec);
  for (auto& [dest, r] : last) {
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, r->fetch, sizeof(fetch));
    tb2::native::HostTexture host;
    std::vector<uint8_t> rgba;
    if (!tb2::native::ConvertTexture(base, fetch, host, nullptr) ||
        !tb2::native::DecodeToRgba8(host, 0, 0, rgba))
      continue;
    // As a shader samples it: through the fetch constant's swizzle (back to
    // the render target's component order).
    for (size_t i = 0; i < rgba.size(); i += 4) {
      uint8_t texel[6] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3], 0, 255};
      for (uint32_t c = 0; c < 4; ++c) rgba[i + c] = texel[std::min<uint32_t>((fetch.swizzle >> (3 * c)) & 7, 5)];
    }
    if (host.format == VK_FORMAT_R16G16B16A16_SFLOAT)
      std::ofstream(g_emu_pending_dir / fmt::format("emu_resolve_{:02}_{:08X}_{}x{}.rgba16f",
                                                    r->index, dest << 12, host.levels[0].width,
                                                    host.levels[0].height),
                    std::ios::binary)
          .write(reinterpret_cast<const char*>(host.data.data()),
                 std::streamsize(size_t(host.levels[0].row_pitch) * host.levels[0].block_rows));
    WritePng(g_emu_pending_dir / fmt::format("emu_resolve_{:02}_{:08X}.png", r->index, dest << 12),
               host.levels[0].width, host.levels[0].height, rgba);
  }
  g_emu_pending.clear();
}

void EndNativeFrame() {
  if (!g_render_frame) return;
  g_render_frame = false;
  std::string error;
  if (!g_renderer->EndFrame(g_native_resolves, &error)) g_native_status += "; frame failed: " + error;
  g_native_stats = g_renderer->stats();
  if (rex::cvar::GetFlagByName("readback_resolve") == "full") {
    g_emu_pending = std::move(g_frame_resolves);
    // WriteFrame numbers this capture next.
    g_emu_pending_dir = g_out_dir / fmt::format("native_capture_{:03}_native", g_serial + 1);
  }
  g_frame_resolves.clear();
}

void WriteNative(std::ofstream& out, const std::filesystem::path& dir) {
  if (g_native_status.empty()) return;
  out << "\n## native renderer (phase 3)\n" << g_native_status << '\n';
  if (g_renderer_failed) return;
  const auto& s = g_native_stats;
  out << fmt::format(
      "draws: {} drawn, {} skipped, {} with unknown shaders; {} clears, {} resolves, {} passes\n"
      "pipelines created: {}; textures uploaded: {} ({} bytes), {} failed, {} uses of resolved "
      "textures, {} signed texture bindings; vertex data {} bytes, constants {} bytes; literal "
      "table entries {}\n",
      s.draws - s.draws_skipped, s.draws_skipped, g_native_unknown_shaders, s.clears, s.resolves,
      s.render_passes, s.pipelines_created, s.textures_uploaded, s.upload_bytes,
      s.texture_failures, s.textures_from_resolves, s.signed_texture_bindings, s.vertex_bytes, s.uniform_bytes,
      g_native_literal_entries);
  out << "registers from the emulator at the frame start (outside the shadow, nonzero):";
  for (auto& r : g_native_seeded) out << ' ' << r;
  out << "\nregisters outside the shadow set by the packets (last values):";
  for (auto [reg, value] : g_ring_regs) out << fmt::format(" {:04X}={:08X}", reg, value);
  out << '\n';
  for (auto& n : s.notes) out << "  " << n << '\n';
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (const auto& r : g_native_resolves) {
    out << fmt::format("resolve {} -> {:08X}: {}x{}{}{}\n", r.index, r.dest_base, r.width,
                       r.height, r.note.empty() ? "" : " - ", r.note);
    if (!r.rgba.empty())
      WritePng(dir / fmt::format("resolve_{:02}_{:08X}.png", r.index, r.dest_base), r.width,
               r.height, r.rgba);
    if (!r.rgba16f.empty())
      std::ofstream(dir / fmt::format("resolve_{:02}_{:08X}_{}x{}.rgba16f", r.index, r.dest_base,
                                      r.width, r.height),
                    std::ios::binary)
          .write(reinterpret_cast<const char*>(r.rgba16f.data()), std::streamsize(r.rgba16f.size()));
  }
  out << fmt::format("written to {}\n", dir.string());
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
  for (const RegWrite& w : parsed.regs) {
    g_mirror[w.reg] = {w.value, {w.packet, n, 'D', w.via}};
    NoteRingRegister(w.reg, w.value);
  }
  for (auto& [op, cnt] : parsed.opcodes) f.opcodes[op] += cnt;
  CollectResources(g, a.device, parsed);
  TrackDrawShaders(g, a.device, parsed,
                   parsed.draws.empty() ? 0 : parsed.draws.back().initiator & 0x3F);
  CheckGeometry(g, a.device, parsed);
  NativeDraw(g, a.device, parsed);

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
  g_base = base;
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  const int state = g_state.load();
  if (state == 2) {
    if (device) CatchUp(g, RingPos(g, device));
    g_frame.events.push_back("S swap");
    EndNativeFrame();
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
      g_ring_regs.clear();
      g_prev_hashes.clear();
      g_current_shader[0] = g_current_shader[1] = 0;
      g_pending = Parsed{};
      g_ring_last = 0;
    }
    g_frame = Frame{};
    g_pending = Parsed{};
    ++g_frame_index;
    g_ring_last = 0;  // set after the swap: it moves to a new ring segment itself
    g_state.store(2);
    BeginNativeFrame(base);
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
  // After the captured frame's Swap, before the next frame's resolves
  // overwrite anything. Swap returns before the GPU has processed the frame's
  // last resolves (the front buffer was still the previous frame's), so give
  // it time: this only runs for comparison captures.
  if (!g_emu_pending.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard lock(g_mutex);
    WriteEmulatorResolves(base);
  }
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
      ctx.r10.u32, ctx.f1.f64, Targets(g, device)) +
      (ctx.r6.u32 ? fmt::format(" | dest fetch {:08X} {:08X} {:08X}", g.U32(ctx.r6.u32 + 28),
                                g.U32(ctx.r6.u32 + 32), g.U32(ctx.r6.u32 + 36))
                  : std::string()));
  if (const uint32_t dest = ctx.r6.u32)
    g_frame.resolve_dest_bases.push_back(GpuAddress(g.U32(dest + 28 + 4) & ~0xFFFu) >> 12);
  if (g_render_frame && ctx.r6.u32) {
    FillRegisters(g, device);
    int32_t rect[4];
    if (const uint32_t r = ctx.r5.u32)
      for (int i = 0; i < 4; ++i) rect[i] = int32_t(g.U32(r + i * 4));
    // The texture object's fetch constant holds CPU addresses.
    uint32_t fetch[6];
    for (int i = 0; i < 6; ++i) fetch[i] = g.U32(ctx.r6.u32 + 28 + i * 4);
    fetch[1] = GpuAddress(fetch[1] & ~0xFFFu) | (fetch[1] & 0xFFF);
    if (fetch[5] & ~0xFFFu) fetch[5] = GpuAddress(fetch[5] & ~0xFFFu) | (fetch[5] & 0xFFF);
    float clear_color[4] = {};
    if (const uint32_t c = ctx.r10.u32)
      for (int i = 0; i < 4; ++i) clear_color[i] = std::bit_cast<float>(g.U32(c + i * 4));
    EmulatorResolve& er = g_frame_resolves.emplace_back();
    er.index = g_renderer->stats().resolves;
    std::memcpy(er.fetch, fetch, sizeof(fetch));
    int32_t point[2];
    if (const uint32_t p = ctx.r7.u32)
      for (int i = 0; i < 2; ++i) point[i] = int32_t(g.U32(p + i * 4));
    g_renderer->Resolve(*g_regs, ctx.r4.u32, ctx.r5.u32 ? rect : nullptr,
                        ctx.r7.u32 ? point : nullptr, fetch, clear_color, float(ctx.f1.f64), 0);
  }
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
  if (g_render_frame) {
    FillRegisters(g, device);
    g_renderer->Clear(*g_regs, ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r9.u32,
                      BoundSurfaces(g, device));
  }
  __imp__sub_823C5C28(ctx, base);
  CatchUp(g, RingPos(g, device), 'C');
}

// sub_823B9308 BeginTiling(device, flags, count, tile rects, clear color, [f1]
// z, stencil) / sub_823B9678 EndTiling(device, resolve flags, resolve rects,
// dest texture, clear color, [f1] z, stencil, params): predicated tiling
// (EDRAM too small for the 4x MSAA scene). Logs the tile rectangles and every
// packet they write.
namespace {

std::string Rects(const Guest& g, uint32_t ptr, uint32_t count) {
  std::string r;
  for (uint32_t i = 0; ptr && i < count && i < 8; ++i)
    r += fmt::format(" ({},{})-({},{})", int32_t(g.U32(ptr + i * 16)), int32_t(g.U32(ptr + i * 16 + 4)),
                     int32_t(g.U32(ptr + i * 16 + 8)), int32_t(g.U32(ptr + i * 16 + 12)));
  return r;
}

template <void (*Original)(PPCContext&, uint8_t*)>
void TilingHook(PPCContext& ctx, uint8_t* base, char tag, std::string event) {
  std::lock_guard lock(g_mutex);
  Guest g{base};
  const uint32_t device = ctx.r3.u32;
  CatchUp(g, RingPos(g, device));
  g_frame.events.push_back(std::move(event) + " | " + Targets(g, device));
  g_log_packets = true;
  Original(ctx, base);
  Scan(g, RingPos(g, device));
  g_log_packets = false;
  CatchUp(g, RingPos(g, device), tag);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_823B9308);
extern "C" REX_FUNC(sub_823B9308) {
  TB2_TRACE_HOOKED(0x823B9308);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_823B9308(ctx, base);
  }
  Guest g{base};
  g_tiling = true;
  TilingHook<__imp__sub_823B9308>(
      ctx, base, 'T',
      fmt::format("T begin tiling flags {:08X} count {} rects{} clear {:08X} z {} stencil {}",
                  ctx.r4.u32, ctx.r5.u32, Rects(g, ctx.r6.u32, ctx.r5.u32), ctx.r7.u32, ctx.f1.f64,
                  ctx.r8.u32));
}

extern "C" REX_FUNC(__imp__sub_823B9678);
extern "C" REX_FUNC(sub_823B9678) {
  TB2_TRACE_HOOKED(0x823B9678);
  if (g_state.load(std::memory_order_relaxed) != 2) {
    [[clang::musttail]] return __imp__sub_823B9678(ctx, base);
  }
  g_tiling = true;  // its resolves are still tiled
  TilingHook<__imp__sub_823B9678>(
      ctx, base, 'E',
      fmt::format("E end tiling resolve flags {:08X} rects {:08X} dest {:08X} clear {:08X} z {} "
                  "stencil {} params {:08X}",
                  ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r8.u32,
                  ctx.r9.u32));
  g_tiling = false;
}
