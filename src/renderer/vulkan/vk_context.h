#pragma once
//
// fable2 :: renderer :: vulkan :: VkContext
//
// The custom renderer's Vulkan foundation (workstream W1). A single owned
// instance + logical device, a VMA allocator, the on-disk/in-memory pipeline
// cache, a GPU-memory tracker, a SPIRV-Tools context, and a capability struct
// (queried once) that the rest of the renderer reads to pick code paths.
//
// VkContext is deliberately independent of the ReXGlue SDK's ui::vulkan
// provider so the renderer can own its own device and presentation path. It
// is created by Fable2VulkanGraphicsSystem (and the standalone smoke test).
//
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "memory_tracker.h"
#include "pipeline_cache.h"
#include "spirv_tools.h"
#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

// A snapshot of the instance + physical-device capabilities, queried once at
// VkContext::Create. The renderer reads this to select features (dynamic
// rendering, 16-bit storage, timeline semaphores, ...) and to emit a precise
// "unsupported" diagnostic for the cross-system robustness story.
struct Capabilities {
  // Instance.
  uint32_t apiVersion = 0;
  std::vector<std::string> instanceExtensions;
  bool validationEnabled = false;

  // Physical device.
  uint32_t deviceApiVersion = 0;
  uint32_t driverVersion = 0;
  uint32_t vendorId = 0;
  uint32_t deviceId = 0;
  std::string deviceName;
  VkDriverId driverId = static_cast<VkDriverId>(0);

  // Queue families.
  int graphicsFamily = -1;
  int computeFamily = -1;
  int transferFamily = -1;  // -1 when graphics family is reused for transfers
  int graphicsQueueCount = 0;
  int computeQueueCount = 0;

  // Features (the subset the renderer branches on).
  bool core11 = false, core12 = false, core13 = false;
  bool dynamicRendering = false;
  bool timelineSemaphore = false;
  bool sync2 = false;
  bool spirv14 = false;
  bool shaderFloat16 = false;
  bool shaderInt16 = false;
  bool bufferDeviceAddress = false;
  bool descriptorIndexing = false;
  bool robustness2 = false;
  bool maintenance4 = false;
  bool memoryBudget = false;
  bool dedicatedAllocation = false;
  bool shaderStencilExport = false;
  bool fragmentShaderInterlock = false;
  bool nonSeamlessCubeMap = false;
  bool customBorderColor = false;
  bool ycbcrConversion = false;
  VkPhysicalDeviceFeatures features = {};

  // Limits the renderer relies on.
  uint32_t maxImageDimension2D = 0;
  uint32_t maxBoundDescriptorSets = 0;
  uint32_t maxColorAttachments = 0;
  VkDeviceSize maxStorageBufferRange = 0;
  VkDeviceSize maxUniformBufferRange = 0;
  VkDeviceSize minUniformBufferOffsetAlignment = 0;
  VkDeviceSize minStorageBufferOffsetAlignment = 0;
  VkDeviceSize nonCoherentAtomSize = 0;
  uint32_t memoryTypeCount = 0;

  // MSAA sample-count support (from VkPhysicalDeviceLimits). Bit flags of
  // VkSampleCountFlagBits. The renderer uses these to pick a supported sample
  // count for color / depth / MSAA render passes (W3).
  VkSampleCountFlags msaaColorSamples = 0;      // framebufferColorSampleCounts
  VkSampleCountFlags msaaDepthSamples = 0;      // framebufferDepthSampleCounts
  VkSampleCountFlags msaaStencilSamples = 0;    // framebufferStencilSampleCounts
  VkSampleCountFlags msaaNoAttachmentSamples = 0;  // framebufferNoAttachmentsSampleCounts

  // The largest supported MSAA color sample count that is at least `requested`
  // (rounded down to the nearest supported 1/2/4/8/16); falls back to 1 when
  // none of 2/4/8/16 are supported. Returns a power-of-two sample count.
  VkSampleCountFlagBits ClampMsaa(VkSampleCountFlagBits requested) const;

  // Presentation.
  bool presentationSupported = false;

  // Device extensions that ended up enabled.
  std::vector<std::string> deviceExtensions;

  // True if every capability the renderer requires is present.
  bool IsSufficient(std::vector<std::string>* missing) const;
};

struct VkContextConfig {
  bool withPresentation = true;  // create a surface/swapchain-capable device
  bool withValidation = false;   // enable VK_LAYER_KHRONOS_validation if present
  int graphicsQueueCount = 1;    // start at 1 graphics queue
  int computeQueueCount = 1;     // start at 1 compute queue
  bool separateTransferQueue = false;  // otherwise reuse the graphics family
  std::string forcedDeviceName;  // "" = auto-select the best discrete device
  std::string pipelineCachePath;  // "" = in-memory only (no disk cache)
  uint64_t memoryBudgetBytes = 0;  // 0 = no hard budget (report only)
  spv_target_env spirvEnv = SPV_ENV_VULKAN_1_1_SPIRV_1_4;
  const char* applicationName = "fable2-renderer";
  uint32_t apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);
  const char* loaderName = "";  // optional custom Vulkan loader path
};

class VkContext {
 public:
  ~VkContext();
  VkContext(const VkContext&) = delete;
  VkContext& operator=(const VkContext&) = delete;

  // Create the full context. On success `out` is valid. On failure `err`
  // describes precisely why (and `missing` in caps may list absent
  // capabilities), so the caller can fall back to the stock renderer.
  static bool Create(const VkContextConfig& config, std::unique_ptr<VkContext>& out,
                     std::string& err);

  // Handles.
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
  VkDevice device() const { return device_; }
  VkQueue graphicsQueue() const { return graphicsQueue_; }
  VkQueue computeQueue() const { return computeQueue_; }
  VkQueue transferQueue() const { return transferQueue_; }
  VmaAllocator vma() const { return vma_; }
  VkPipelineCache pipelineCache() const { return pipelineCache_ ? pipelineCache_->handle()
                                                                : VK_NULL_HANDLE; }
  SpirvToolsContext& spirvTools() { return spirvTools_; }
  Functions& functions() { return fn_; }
  const Functions& functions() const { return fn_; }
  const Capabilities& caps() const { return caps_; }
  const VkContextConfig& config() const { return config_; }
  MemoryTracker& memory() { return memory_; }
  const MemoryTracker& memory() const { return memory_; }
  const std::string& pipelineCachePath() const { return config_.pipelineCachePath; }

  // --- VMA allocation helpers (tracked in the MemoryTracker). ---
  // Returns false (and sets err) on failure or budget trip.
  bool AllocateBuffer(VkBuffer* outBuffer, VmaAllocation* outAlloc, VkDeviceSize size,
                      VmaMemoryUsage usage, VkBufferUsageFlags usageFlags,
                      VkMemoryPropertyFlags memProps, void** hostPtr, std::string& err);
  void FreeBuffer(VkBuffer buffer, VmaAllocation alloc);

  bool AllocateImage(VkImage* outImage, VmaAllocation* outAlloc, const VkImageCreateInfo& cbi,
                     VmaMemoryUsage usage, const char* label, std::string& err);
  void FreeImage(VkImage image, VmaAllocation alloc);

  // --- pipeline cache (disk + memory). ---
  bool LoadPipelineCache(std::string& err);
  bool SavePipelineCache(std::string& err);
  void StorePipelineKey(const std::string& key, const void* data, size_t size);
  const std::vector<uint8_t>* LookupPipelineKey(const std::string& key);
  PipelineCache::Stats PipelineCacheStats() const;

  // Debug object naming.
  void SetObjectName(VkBuffer buffer, const char* name);
  void SetObjectName(VkImage image, const char* name);
  void SetObjectName(VmaAllocation alloc, const char* name);

  // Wait for all in-flight work (used by the smoke test + shutdown).
  void WaitIdle();

 private:
  VkContext() = default;
  bool CreateInstance(std::string& err);
  bool SelectDevice(std::string& err);
  bool CreateDevice(std::string& err);
  bool CreateVma(std::string& err);
  bool CreatePipelineCache(std::string& err);

  Functions fn_;
  VkContextConfig config_{};
  Capabilities caps_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphicsQueue_ = VK_NULL_HANDLE;
  VkQueue computeQueue_ = VK_NULL_HANDLE;
  VkQueue transferQueue_ = VK_NULL_HANDLE;
  VmaAllocator vma_ = VK_NULL_HANDLE;
  std::unique_ptr<PipelineCache> pipelineCache_;
  SpirvToolsContext spirvTools_;
  MemoryTracker memory_;

  // Debug utils messenger (optional).
  VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
  // The queue families actually used (kept for VMA + sync setup).
  uint32_t gfxFamily_ = 0, computeFamily_ = 0, transferFamily_ = 0;
  VmaAllocatorCreateFlags allocatorFlags_ = 0;
  VkPhysicalDeviceMemoryProperties memoryProperties_{};  // for host-visibility checks
};

}  // namespace fable2::renderer::vulkan
