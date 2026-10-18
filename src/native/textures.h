// thebigs2 - native renderer: Xenos textures -> host (Vulkan) textures
// (docs/native-renderer.md, phase 2).
//
// Converts a texture described by its fetch constant from guest memory: untiles
// it, swaps the byte order, converts the formats Vulkan has no equivalent for,
// and lays out all levels and array layers linearly, ready for
// vkCmdCopyBufferToImage. Sizes and level/mip-tail placement come from the
// SDK's texture_util (the same code its texture cache uses).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include <rex/graphics/xenos.h>

namespace tb2::native {

struct HostTexture {
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
  uint32_t width = 0, height = 0, depth = 1, layers = 1;
  uint32_t block_width = 1, block_height = 1, bytes_per_block = 0;  // host format
  struct Level {
    uint32_t width, height, depth;
    size_t offset;           // in data: layer 0 of this level, layers follow
    uint32_t row_pitch;      // bytes per host block row
    uint32_t block_rows;     // block rows per depth slice
    size_t layer_bytes;      // all depth slices of one layer
  };
  std::vector<Level> levels;  // 0..mip_max_level
  std::vector<uint8_t> data;
};

// Vulkan format a guest texture format converts to, or VK_FORMAT_UNDEFINED.
VkFormat HostTextureFormat(rex::graphics::xenos::TextureFormat format);

// Converts the texture of a fetch constant. memory: guest memory base (guest
// physical address p is at memory + 0xA0000000 + p). False with a reason if the
// texture can't be converted.
bool ConvertTexture(const uint8_t* memory, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                    HostTexture& out, std::string* error);

// RGBA8 preview of one level / layer (decodes the compressed formats), for
// debugging. False for formats it doesn't know.
bool DecodeToRgba8(const HostTexture& texture, uint32_t level, uint32_t layer,
                   std::vector<uint8_t>& rgba);

}  // namespace tb2::native
