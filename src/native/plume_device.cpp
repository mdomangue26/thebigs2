// thebigs2 - native renderer: Plume on the SDK's Vulkan device (see
// plume_device.h).

#include "plume_device.h"

#include <cstring>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/provider.h>

#include <plume_vulkan.h>

REXCVAR_DEFINE_BOOL(native_plume_selftest, false, "The Bigs 2/Debug",
                    "At startup, wrap the SDK's Vulkan device with Plume and check a clear, a "
                    "readback and a buffer copy (native renderer)");

namespace tb2::native {

PlumeDevice::~PlumeDevice() {
  device.reset();
  render_interface.reset();
}

std::unique_ptr<PlumeDevice> CreatePlumeDevice(const rex::ui::vulkan::VulkanDevice* device,
                                               std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return nullptr;
  };
  if (!device) return fail("no SDK Vulkan device");
  const auto* instance = device->vulkan_instance();
  const auto& props = device->properties();
  // The translated shaders' SystemConstants block isn't std140
  // (src/native/xenos/README.md).
  if (!props.scalarBlockLayout) return fail("the device has no scalarBlockLayout");

  const uint32_t family = device->queue_family_graphics_compute();
  auto& queue = *device->queue_families()[family].queues[0];

  plume::VulkanExternalDeviceDesc desc;
  desc.getInstanceProcAddr = instance->functions().vkGetInstanceProcAddr;
  desc.instance = instance->instance();
  desc.instanceApiVersion = instance->api_version();
  desc.physicalDevice = device->physical_device();
  desc.device = device->device();
  desc.queueFamilyIndex = family;
  desc.queue = queue.queue;
  desc.queueMutex = &queue.mutex;
  desc.geometryShader = props.geometryShader;
  desc.scalarBlockLayout = props.scalarBlockLayout;
  desc.nullDescriptor = props.nullDescriptor;
  desc.samplerMirrorClampToEdge = props.samplerMirrorClampToEdge;

  auto result = std::make_unique<PlumeDevice>();
  result->render_interface = plume::CreateVulkanInterface(desc);
  if (!result->render_interface) return fail("Plume: no interface over the SDK instance");
  result->device = result->render_interface->createDevice("");
  if (!result->device) return fail("Plume: no device over the SDK device");
  const auto& d = result->device->getDescription();
  REXLOG_INFO("[native] Plume on the SDK device: {}, {} MB VRAM", d.name,
              d.dedicatedVideoMemory >> 20);
  return result;
}

bool PlumeSelfTest(PlumeDevice& plume_device) {
  using namespace plume;
  RenderDevice& device = *plume_device.device;
  auto fail = [](const std::string& message) {
    REXLOG_ERROR("[native] Plume self-test failed: {}", message);
    return false;
  };

  constexpr uint32_t kSize = 64;
  constexpr RenderFormat kFormat = RenderFormat::R8G8B8A8_UNORM;
  constexpr uint32_t kBytes = kSize * kSize * 4;

  auto queue = device.createCommandQueue(RenderCommandListType::DIRECT);
  auto list = queue->createCommandList();
  auto fence = device.createCommandFence();
  auto target = device.createTexture(
      RenderTextureDesc::Texture2D(kSize, kSize, 1, kFormat, RenderTextureFlag::RENDER_TARGET));
  auto readback = device.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
  auto upload = device.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));
  auto copy = device.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
  if (!queue || !list || !fence || !target || !readback || !upload || !copy)
    return fail("object creation");
  const RenderTexture* attachments[] = {target.get()};
  auto framebuffer = device.createFramebuffer(RenderFramebufferDesc(attachments, 1));
  if (!framebuffer) return fail("framebuffer creation");

  auto* up = static_cast<uint8_t*>(upload->map());
  for (uint32_t i = 0; i < kBytes; ++i) up[i] = uint8_t(i * 7);
  upload->unmap();

  // (0.25, 0.5, 0.75, 1) -> 64, 128, 191, 255
  list->begin();
  list->barriers(RenderBarrierStage::GRAPHICS,
                 RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(framebuffer.get());
  list->clearColor(0, RenderColor(0.25f, 0.5f, 0.75f, 1.0f));
  list->setFramebuffer(nullptr);
  list->barriers(RenderBarrierStage::COPY,
                 RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(
      RenderTextureCopyLocation::PlacedFootprint(readback.get(), kFormat, kSize, kSize, 1, kSize),
      RenderTextureCopyLocation::Subresource(target.get()));
  list->copyBuffer(copy.get(), upload.get());
  list->end();
  const RenderCommandList* lists[] = {list.get()};
  queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());

  const auto* pixels = static_cast<const uint8_t*>(readback->map());
  uint32_t bad_pixels = 0;
  for (uint32_t i = 0; i < kSize * kSize; ++i) {
    const uint8_t* p = pixels + i * 4;
    if (std::abs(p[0] - 64) > 1 || std::abs(p[1] - 128) > 1 || std::abs(p[2] - 191) > 1 ||
        p[3] != 255)
      ++bad_pixels;
  }
  const std::string first = fmt::format("{} {} {} {}", pixels[0], pixels[1], pixels[2], pixels[3]);
  readback->unmap();
  const auto* copied = static_cast<const uint8_t*>(copy->map());
  uint32_t bad_bytes = 0;
  for (uint32_t i = 0; i < kBytes; ++i) bad_bytes += copied[i] != uint8_t(i * 7);
  copy->unmap();

  if (bad_pixels)
    return fail(fmt::format("{} of {} cleared pixels wrong (first: {})", bad_pixels,
                            kSize * kSize, first));
  if (bad_bytes) return fail(fmt::format("{} of {} copied bytes wrong", bad_bytes, kBytes));
  REXLOG_INFO("[native] Plume self-test passed (clear {} + readback, buffer copy)", first);
  return true;
}

void PlumeStartupCheck() {
  if (!REXCVAR_GET(native_plume_selftest)) return;
  auto* graphics = rex::system::kernel_state()->emulator()->graphics_system();
  auto* provider =
      graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
  if (!provider) {
    REXLOG_ERROR("[native] Plume self-test: the GPU backend isn't Vulkan");
    return;
  }
  std::string error;
  auto device = CreatePlumeDevice(provider->vulkan_device(), &error);
  if (!device) {
    REXLOG_ERROR("[native] Plume self-test: {}", error);
    return;
  }
  PlumeSelfTest(*device);
}

}  // namespace tb2::native
