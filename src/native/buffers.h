// thebigs2 - native renderer: draw geometry (docs/native-renderer.md, phase 2).
//
// Vertex data stays as it is in guest memory: the translated vertex shaders
// fetch and byte-swap it themselves (vfetch, from a storage buffer addressed by
// the fetch constants), so vertex buffers are uploaded raw. Index buffers are
// byte-swapped, and primitive types Vulkan lacks are converted to triangle
// lists. The game's draws (captures, 2026-10-17): triangle lists and strips
// with 16-bit indices from memory (no primitive restart), and auto-indexed
// triangle lists, strips and single quads.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace tb2::native {

// A guest draw as the draw packet describes it.
struct GuestDraw {
  uint32_t initiator;      // VGT_DRAW_INITIATOR
  uint32_t index_base;     // physical address of the indices (DMA source)
  uint32_t index_size;     // DMA size dword: count in bits 0-23, endian in 30-31
};

struct HostDraw {
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  uint32_t count = 0;            // vertices (non-indexed) or indices
  bool indexed = false;
  bool index32 = false;
  std::vector<uint8_t> indices;  // host byte order; generated for converted auto-indexed draws
  uint32_t min_index = 0, max_index = 0;  // of the guest indices (indexed draws)
};

// memory: guest memory base (physical p at memory + 0xA0000000 + p).
bool ConvertDraw(const uint8_t* memory, const GuestDraw& draw, HostDraw& out, std::string* error);

}  // namespace tb2::native
