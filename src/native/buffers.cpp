// thebigs2 - native renderer: draw geometry (see buffers.h).

#include "buffers.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace tb2::native {

namespace {

// xenos::PrimitiveType
enum : uint32_t {
  kPointList = 1, kLineList = 2, kLineStrip = 3, kTriangleList = 4, kTriangleFan = 5,
  kTriangleStrip = 6, kRectangleList = 8, kQuadList = 13,
};

uint32_t Swap(uint32_t v, uint32_t endian, bool is32) {
  switch (endian) {
    case 1:  // 8in16
      return is32 ? ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu)
                  : uint16_t((v << 8) | ((v >> 8) & 0xFF));
    case 2:  // 8in32
      return is32 ? __builtin_bswap32(v) : v;
    case 3:  // 16in32
      return is32 ? (v << 16) | (v >> 16) : v;
    default:
      return v;
  }
}

}  // namespace

bool ConvertDraw(const uint8_t* memory, const GuestDraw& draw, HostDraw& out, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  out = HostDraw{};
  const uint32_t prim = draw.initiator & 0x3F, source = (draw.initiator >> 6) & 3;
  const uint32_t count = draw.initiator >> 16;
  const bool is32 = (draw.initiator >> 11) & 1;
  bool quads = false;
  switch (prim) {
    case kPointList: out.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case kLineList: out.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case kLineStrip: out.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case kTriangleList: out.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case kTriangleFan: out.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case kTriangleStrip: out.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case kQuadList:
      out.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    default: return fail(fmt::format("primitive type {} not supported", prim));
  }

  std::vector<uint32_t> guest;  // guest indices in host order (indexed draws)
  if (source == 0) {  // DMA
    const uint32_t bytes = count * (is32 ? 4 : 2);
    if (!draw.index_base || uint64_t(draw.index_base) + bytes > 0x20000000)
      return fail(fmt::format("index buffer {:08X}+{} outside physical memory", draw.index_base,
                              bytes));
    const uint8_t* src = memory + 0xA0000000u + draw.index_base;
    const uint32_t endian = draw.index_size >> 30;
    guest.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = 0;
      if (is32) {
        std::memcpy(&v, src + i * 4, 4);
      } else {
        uint16_t h;
        std::memcpy(&h, src + i * 2, 2);
        v = h;
      }
      guest[i] = Swap(v, endian, is32);
    }
    if (count) {
      auto [lo, hi] = std::minmax_element(guest.begin(), guest.end());
      out.min_index = *lo, out.max_index = *hi;
    }
  } else if (source != 2) {  // auto-indexed is 2; 1 = immediate (DRAW_INDX_2)
    return fail("immediate indices not supported");
  }

  if (quads) {  // 0 1 2 3 -> 0 1 2, 0 2 3
    const uint32_t n = count / 4;
    std::vector<uint32_t> list;
    list.reserve(n * 6);
    for (uint32_t q = 0; q < n; ++q) {
      uint32_t v[4];
      for (uint32_t k = 0; k < 4; ++k) v[k] = guest.empty() ? q * 4 + k : guest[q * 4 + k];
      list.insert(list.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
    if (guest.empty() && n) out.min_index = 0, out.max_index = n * 4 - 1;
    guest = std::move(list);
  } else if (guest.empty()) {
    out.count = count;
    return true;
  }

  out.indexed = true;
  out.count = uint32_t(guest.size());
  out.index32 = is32 || out.max_index > 0xFFFF;
  out.indices.resize(guest.size() * (out.index32 ? 4 : 2));
  for (size_t i = 0; i < guest.size(); ++i) {
    if (out.index32) {
      std::memcpy(&out.indices[i * 4], &guest[i], 4);
    } else {
      const uint16_t h = uint16_t(guest[i]);
      std::memcpy(&out.indices[i * 2], &h, 2);
    }
  }
  return true;
}

}  // namespace tb2::native
