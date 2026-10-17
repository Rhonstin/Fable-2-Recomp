#include <cstdio>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>

int main() {
  rex::InitLoggingEarly();
  rex::cvar::ApplyEnvironment();
  auto graphics = rex::system::LoadGpuPlugin("xenos", "vulkan");
  if (!graphics) {
    std::fprintf(stderr, "REXGLUE_VULKAN_PLUGIN_FAILED\n");
    return 3;
  }
  auto provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
  if (!provider) {
    std::fprintf(stderr, "REXGLUE_VULKAN_DEVICE_FAILED\n");
    return 1;
  }
  VkPhysicalDeviceProperties properties{};
  const auto* instance = provider->vulkan_instance();
  instance->functions().vkGetPhysicalDeviceProperties(
      provider->vulkan_device()->physical_device(), &properties);
  std::printf("GPU=%s vendor=0x%x Vulkan=%u.%u.%u\n", properties.deviceName,
              properties.vendorID, VK_VERSION_MAJOR(properties.apiVersion),
              VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
    std::fprintf(stderr, "Software rendering does not validate the Odin GPU\n");
    return 2;
  }
  std::puts("REXGLUE_VULKAN_DEVICE_OK");
  return 0;
}
