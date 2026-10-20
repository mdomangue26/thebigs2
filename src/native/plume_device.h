// thebigs2 - native renderer: Plume on the SDK's Vulkan device
// (docs/native-renderer.md).
//
// The renderer is written against Plume (src/native/plume, README.md there).
// Plume normally creates its own instance and device; with the local
// VulkanExternalDeviceDesc patch it wraps the SDK's instead, so the renderer's
// images can be handed to the SDK's presenter (ImGui, gamma ramp, pacing) and
// both submit to the same queue under the SDK's queue mutex.

#pragma once

#include <memory>
#include <string>

namespace plume {
struct RenderDevice;
struct RenderInterface;
}  // namespace plume
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace tb2::native {

struct PlumeDevice {
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;

  ~PlumeDevice();  // the device goes before the interface
};

// Wraps the SDK's device. The SDK device must outlive the result.
std::unique_ptr<PlumeDevice> CreatePlumeDevice(const rex::ui::vulkan::VulkanDevice* device,
                                               std::string* error);

// --native_plume_selftest: clears a render target, copies it to a readback
// buffer, and checks the pixels; also checks a buffer copy. Logs the result.
bool PlumeSelfTest(PlumeDevice& device);

// Runs PlumeSelfTest on the SDK's device if --native_plume_selftest is set.
void PlumeStartupCheck();

}  // namespace tb2::native
