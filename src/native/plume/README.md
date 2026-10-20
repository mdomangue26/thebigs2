# Plume (copied, with local changes)

The native renderer (`docs/native-renderer.md`) draws through
[Plume](https://github.com/renderbag/plume), the rendering API RT64 is built
on: one interface over Vulkan, D3D12 and Metal. Only the Vulkan backend is
built (static library `tb2_plume`), against the SDK's Vulkan headers and with
SDL3 windows.

- Source: `renderbag/plume` at `e48a55b` (2026-10-12): `plume_render_interface*.h`,
  `plume_vulkan.{h,cpp}`. Licence: `LICENSE` (MIT).
- `contrib/volk/`: `volk.{h,c}` from `zeux/volk` at `be3dbd4` (Plume's
  submodule). Licence: `contrib/volk/LICENSE.md` (MIT).
- `contrib/VulkanMemoryAllocator/`: `vk_mem_alloc.h` from
  `GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator` at `29b35ea` (Plume's
  submodule). Licence: `contrib/VulkanMemoryAllocator/LICENSE.txt` (MIT).

Changes from upstream (marked `thebigs2:` in the code):

- **External device** (`VulkanExternalDeviceDesc`,
  `CreateVulkanInterface(const VulkanExternalDeviceDesc &)`): Plume wraps a
  VkInstance / VkDevice / VkQueue it didn't create (the SDK's, see
  `../plume_device.cpp`) instead of creating its own, and never destroys them.
  volk is loaded through the owner's `vkGetInstanceProcAddr`. Features can't be
  queried from a device, so the owner says which ones it enabled. All command
  queues map to the owner's one queue.
- **Queue mutex**: `VulkanQueue::mutex` is a `std::recursive_mutex *` (Plume's
  own, or the SDK's queue mutex for an external device), so Plume's and the
  SDK's submissions don't race.
- **Texture -> buffer copies** in `VulkanCommandList::copyTextureRegion`
  (`vkCmdCopyImageToBuffer`). Upstream only copies buffer -> texture and
  texture -> texture on Vulkan; a readback fell into the texture branch with a
  null destination.
- **For the renderer** (2026-10-20): `RenderFormat::R10G10B10A2_UNORM`;
  `RenderTextureFlag::MUTABLE_FORMAT` (signed views); `RenderTextureViewDesc::
  forceArray` (2D arrays of one layer); `RenderBlend::BLEND_FACTOR_ALPHA` /
  `INV_BLEND_FACTOR_ALPHA` (constant alpha); `dynamicStencilEnabled` /
  `dynamicBlendConstantsEnabled` pipelines with
  `RenderCommandList::setStencilState` / `setBlendConstants`; texture -> buffer
  copies of depth/stencil textures take the depth aspect;
  `RenderTextureViewDesc::stencilAspect` (stencil views for the packed depth
  resolves).

To update: copy the files above from a new upstream checkout (and its
submodules), then reapply the changes (`git diff` this folder against the
previous copy).
