#pragma once
//
// fable2 :: renderer :: vulkan :: Functions
//
// Runtime-resolved table of the Vulkan entry points the custom renderer uses.
//
// Like the ReXGlue SDK, the renderer loads the Vulkan *loader*
// (vulkan-1.dll / libvulkan.so / libvulkan.1.dylib) at runtime and resolves
// every function through vkGetInstanceProcAddr / vkGetDeviceProcAddr. This
// keeps the plugin free of a Vulkan import-library link dependency and works
// on Windows, Linux and macOS with the same code.
//
// Loading is three-phase so functions that need a non-null handle can be
// resolved once the object they depend on exists:
//   Phase 1  LoadGlobal(loader_name)      - loader lib + global/instance-less fns
//   Phase 2  LoadInstance(instance)       - instance-level fns
//   Phase 3  LoadDevice(device)           - device-level fns
//
#include <string>

#include <vulkan/vulkan.h>

#include "vk_dynlib.h"

namespace fable2::renderer::vulkan {

struct Functions {
  DynLib loader;

  // ---- Resolved directly from the loader library (no instance needed). ----
  PFN_vkCreateInstance vkCreateInstance = nullptr;
  PFN_vkDestroyInstance vkDestroyInstance = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
  PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
  PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;

  // ---- Instance level. ----
  PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
  PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;

  // Surface / presentation queries (instance level; need a VkSurfaceKHR).
  PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;

  // ---- Device level. ----
  PFN_vkCreateDevice vkCreateDevice = nullptr;
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;

  // Pipeline cache.
  PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;
  PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
  PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;

  // Memory / resource (needed by VMA and the allocation helpers).
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkUnmapMemory vkUnmapMemory = nullptr;
  PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges = nullptr;
  PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkCmdCopyBuffer vkCmdCopyBuffer = nullptr;

  // Optional device extensions (resolved only when enabled; VMA uses them).
  PFN_vkGetBufferMemoryRequirements2KHR vkGetBufferMemoryRequirements2 = nullptr;
  PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2 = nullptr;
  PFN_vkBindBufferMemory2KHR vkBindBufferMemory2 = nullptr;
  PFN_vkBindImageMemory2KHR vkBindImageMemory2 = nullptr;
  PFN_vkGetDeviceBufferMemoryRequirements vkGetDeviceBufferMemoryRequirements = nullptr;
  PFN_vkGetDeviceImageMemoryRequirements vkGetDeviceImageMemoryRequirements = nullptr;

  // Debug utils (optional).
  PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
  PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;

  // Command pool / sync (used by the foundation smoke test for a real GPU
  // buffer round-trip).
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
  PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
  PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkResetFences vkResetFences = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
  PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
  PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;

  // Presentation (swapchain extension + core image/render-pass resources).
  PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
  PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
  PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
  PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
  PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
  PFN_vkCreateImageView vkCreateImageView = nullptr;
  PFN_vkDestroyImageView vkDestroyImageView = nullptr;
  PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
  PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;

  // Transfer / layout (W3 downscale/resolve + RTT compositing).
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
  PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
  PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;

  // Phase 1: load the loader library and resolve the global functions.
  bool LoadGlobal(const char* loader_name, std::string& err);
  // Phase 2: resolve instance-level functions.
  bool LoadInstance(VkInstance instance, std::string& err);
  // Phase 3: resolve device-level functions.
  bool LoadDevice(VkDevice device, std::string& err);

  bool IsReady() const { return vkCreateInstance && vkCreateDevice; }
};

}  // namespace fable2::renderer::vulkan
