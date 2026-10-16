#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

namespace {
#ifdef _WIN32
constexpr const char* kLoader = "vulkan-1.dll";
#else
constexpr const char* kLoader = "libvulkan.so.1";
#endif
}  // namespace

bool Functions::LoadGlobal(const char* loader_name, std::string& err) {
  err.clear();

  // 1. Load the loader library.
  bool ok = loader.Load(kLoader);
  if (!ok && loader_name && loader_name[0]) {
    ok = loader.Load(loader_name);
  }
  if (!ok) {
    err = "failed to load the Vulkan loader ('" + std::string(kLoader) + "')";
    return false;
  }

  // 2. The two functions resolved directly from the library.
  vkGetInstanceProcAddr = loader.GetSymbolAs<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  if (!vkGetInstanceProcAddr) {
    err = "vkGetInstanceProcAddr not found in the Vulkan loader";
    return false;
  }
  // 3. Global / instance-less functions.
#define FABLE2_GLOBAL(name) \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name))
  FABLE2_GLOBAL(vkCreateInstance);
  FABLE2_GLOBAL(vkDestroyInstance);
  FABLE2_GLOBAL(vkEnumerateInstanceExtensionProperties);
  FABLE2_GLOBAL(vkEnumerateInstanceLayerProperties);
  FABLE2_GLOBAL(vkEnumerateInstanceVersion);
#undef FABLE2_GLOBAL

  if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) {
    err = "failed to resolve global Vulkan functions (vkCreateInstance / "
          "vkEnumerateInstanceExtensionProperties)";
    return false;
  }
  return true;
}

bool Functions::LoadInstance(VkInstance instance, std::string& err) {
  err.clear();
  if (!instance || !vkGetInstanceProcAddr) {
    err = "LoadInstance: no instance / loader ready";
    return false;
  }
#define FABLE2_INST(name) \
  name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name))
  FABLE2_INST(vkGetDeviceProcAddr);
  FABLE2_INST(vkEnumeratePhysicalDevices);
  FABLE2_INST(vkGetPhysicalDeviceProperties);
  FABLE2_INST(vkGetPhysicalDeviceFeatures);
  FABLE2_INST(vkGetPhysicalDeviceQueueFamilyProperties);
  FABLE2_INST(vkGetPhysicalDeviceMemoryProperties);
  FABLE2_INST(vkGetPhysicalDeviceFormatProperties);
  FABLE2_INST(vkCreateDevice);
  FABLE2_INST(vkSetDebugUtilsObjectNameEXT);
  // Surface / presentation queries (resolved even when no surface is created;
  // the caller only uses them when it has a VkSurfaceKHR).
  FABLE2_INST(vkDestroySurfaceKHR);
  FABLE2_INST(vkGetPhysicalDeviceSurfaceSupportKHR);
  FABLE2_INST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  FABLE2_INST(vkGetPhysicalDeviceSurfaceFormatsKHR);
  FABLE2_INST(vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef FABLE2_INST

  if (!vkEnumeratePhysicalDevices || !vkCreateDevice) {
    err = "failed to resolve instance-level Vulkan functions";
    return false;
  }
  return true;
}

bool Functions::LoadDevice(VkDevice device, std::string& err) {
  err.clear();
  if (!device || (!vkGetDeviceProcAddr && !vkGetInstanceProcAddr)) {
    err = "LoadDevice: no device / proc-addr ready";
    return false;
  }
  auto resolve = [this, device](const char* name) -> PFN_vkVoidFunction {
    PFN_vkVoidFunction f = nullptr;
    if (vkGetDeviceProcAddr) {
      f = vkGetDeviceProcAddr(device, name);
    }
    if (!f && vkGetInstanceProcAddr) {
      f = vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
    }
    return f;
  };
#define FABLE2_DEV(name) name = reinterpret_cast<PFN_##name>(resolve(#name))
  FABLE2_DEV(vkDestroyDevice);
  FABLE2_DEV(vkGetDeviceQueue);
  FABLE2_DEV(vkCreatePipelineCache);
  FABLE2_DEV(vkDestroyPipelineCache);
  FABLE2_DEV(vkGetPipelineCacheData);
  FABLE2_DEV(vkAllocateMemory);
  FABLE2_DEV(vkFreeMemory);
  FABLE2_DEV(vkMapMemory);
  FABLE2_DEV(vkUnmapMemory);
  FABLE2_DEV(vkFlushMappedMemoryRanges);
  FABLE2_DEV(vkInvalidateMappedMemoryRanges);
  FABLE2_DEV(vkBindBufferMemory);
  FABLE2_DEV(vkBindImageMemory);
  FABLE2_DEV(vkGetBufferMemoryRequirements);
  FABLE2_DEV(vkGetImageMemoryRequirements);
  FABLE2_DEV(vkCreateBuffer);
  FABLE2_DEV(vkDestroyBuffer);
  FABLE2_DEV(vkCreateImage);
  FABLE2_DEV(vkDestroyImage);
  FABLE2_DEV(vkCmdCopyBuffer);
  // Optional (KHR-suffixed PFN names).
  vkGetBufferMemoryRequirements2 =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements2KHR>(resolve("vkGetBufferMemoryRequirements2KHR"));
  vkGetImageMemoryRequirements2 =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements2KHR>(resolve("vkGetImageMemoryRequirements2KHR"));
  vkBindBufferMemory2 =
      reinterpret_cast<PFN_vkBindBufferMemory2KHR>(resolve("vkBindBufferMemory2KHR"));
  vkBindImageMemory2 =
      reinterpret_cast<PFN_vkBindImageMemory2KHR>(resolve("vkBindImageMemory2KHR"));
  vkGetDeviceBufferMemoryRequirements =
      reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(resolve("vkGetDeviceBufferMemoryRequirements"));
  vkGetDeviceImageMemoryRequirements =
      reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(resolve("vkGetDeviceImageMemoryRequirements"));
  // Sync / command pool.
  FABLE2_DEV(vkCreateCommandPool);
  FABLE2_DEV(vkDestroyCommandPool);
  FABLE2_DEV(vkAllocateCommandBuffers);
  FABLE2_DEV(vkFreeCommandBuffers);
  FABLE2_DEV(vkBeginCommandBuffer);
  FABLE2_DEV(vkEndCommandBuffer);
  FABLE2_DEV(vkResetCommandBuffer);
  FABLE2_DEV(vkCmdBeginRenderPass);
  FABLE2_DEV(vkCmdEndRenderPass);
  FABLE2_DEV(vkQueueSubmit);
  FABLE2_DEV(vkCreateFence);
  FABLE2_DEV(vkDestroyFence);
  FABLE2_DEV(vkResetFences);
  FABLE2_DEV(vkWaitForFences);
  FABLE2_DEV(vkCreateSemaphore);
  FABLE2_DEV(vkDestroySemaphore);
  FABLE2_DEV(vkQueueWaitIdle);
  FABLE2_DEV(vkDeviceWaitIdle);
  // Presentation (swapchain extension + core image / render-pass resources).
  FABLE2_DEV(vkCreateSwapchainKHR);
  FABLE2_DEV(vkGetSwapchainImagesKHR);
  FABLE2_DEV(vkDestroySwapchainKHR);
  FABLE2_DEV(vkAcquireNextImageKHR);
  FABLE2_DEV(vkQueuePresentKHR);
  FABLE2_DEV(vkCreateImageView);
  FABLE2_DEV(vkDestroyImageView);
  FABLE2_DEV(vkCreateRenderPass);
  FABLE2_DEV(vkDestroyRenderPass);
  FABLE2_DEV(vkCreateFramebuffer);
  FABLE2_DEV(vkDestroyFramebuffer);
#undef FABLE2_DEV

  if (!vkAllocateMemory || !vkCreateBuffer || !vkGetDeviceQueue) {
    err = "failed to resolve device-level Vulkan functions";
    return false;
  }
  return true;
}

}  // namespace fable2::renderer::vulkan
