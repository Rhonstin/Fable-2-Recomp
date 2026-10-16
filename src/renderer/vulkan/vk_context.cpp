// Provide the Vulkan Memory Allocator implementation exactly once for this
// translation unit (it is header-only; the SDK's own copy is not compiled into
// this plugin). VMA_STATIC_VULKAN_FUNCTIONS=0 makes VMA call only the
// function pointers we pass via VmaVulkanFunctions, so we do not need to link
// the Vulkan loader import library (we resolve it at runtime).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "vk_context.h"

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/platform.h>

namespace fable2::renderer::vulkan {

namespace {

bool StringHas(const std::vector<std::string>& v, const char* name) {
  return std::find(v.begin(), v.end(), name) != v.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
bool Capabilities::IsSufficient(std::vector<std::string>* missing) const {
  if (missing) missing->clear();
  const auto need = [&](bool ok, const char* what) {
    if (!ok && missing) missing->emplace_back(what);
    return ok;
  };
  bool ok = true;
  ok &= need(deviceApiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0),
             "Vulkan 1.2 device");
  ok &= need(graphicsFamily >= 0, "graphics queue family");
  if (presentationSupported) {
    ok &= need(StringHas(deviceExtensions, "VK_KHR_swapchain"), "VK_KHR_swapchain");
  }
  ok &= need(dynamicRendering, "VK_KHR_dynamic_rendering");
  ok &= need(timelineSemaphore, "VK_KHR_timeline_semaphore");
  ok &= need(maintenance4, "VK_KHR_maintenance4");
  return ok;
}

// ---------------------------------------------------------------------------
// VkContext
// ---------------------------------------------------------------------------
VkContext::~VkContext() {
  if (device_) {
    fn_.vkDeviceWaitIdle(device_);
  }
  if (vma_) {
    vmaDestroyAllocator(vma_);
    vma_ = nullptr;
  }
  if (debugMessenger_ && fn_.vkDestroyDebugUtilsMessengerEXT) {
    fn_.vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    debugMessenger_ = VK_NULL_HANDLE;
  }
  if (device_ && fn_.vkDestroyDevice) {
    fn_.vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (instance_ && fn_.vkDestroyInstance) {
    fn_.vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  spirvTools_.Shutdown();
  fn_.loader.Close();
}

bool VkContext::Create(const VkContextConfig& config, std::unique_ptr<VkContext>& out,
                       std::string& err) {
  out.reset();
  auto ctx = std::unique_ptr<VkContext>(new VkContext());
  ctx->config_ = config;
  ctx->memory_ = MemoryTracker(config.memoryBudgetBytes);

  if (!ctx->fn_.LoadGlobal(config.loaderName, err)) {
    REXGPU_ERROR("fable2-renderer: Vulkan unavailable: {}", err);
    return false;
  }
  if (!ctx->CreateInstance(err)) {
    REXGPU_ERROR("fable2-renderer: Vulkan instance failed: {}", err);
    return false;
  }
  if (!ctx->SelectDevice(err)) {
    REXGPU_ERROR("fable2-renderer: no suitable Vulkan device: {}", err);
    return false;
  }
  if (!ctx->CreateDevice(err)) {
    REXGPU_ERROR("fable2-renderer: Vulkan device failed: {}", err);
    return false;
  }
  if (!ctx->CreateVma(err)) {
    REXGPU_ERROR("fable2-renderer: VMA allocator failed: {}", err);
    return false;
  }
  if (!ctx->CreatePipelineCache(err)) {
    REXGPU_ERROR("fable2-renderer: pipeline cache failed: {}", err);
    return false;
  }

  // SPIRV-Tools: best-effort (the renderer still works without it; W7 uses it).
  {
    std::string sErr;
    if (!ctx->spirvTools_.Initialize(config.spirvEnv, sErr)) {
      REXGPU_INFO("fable2-renderer: SPIRV-Tools unavailable ({}); continuing without it", sErr);
    }
  }

  // Disk pipeline cache: best-effort load at startup.
  if (!config.pipelineCachePath.empty()) {
    std::string pErr;
    ctx->LoadPipelineCache(pErr);
  }

  // Capability summary.
  {
    std::vector<std::string> missing;
    const bool sufficient = ctx->caps_.IsSufficient(&missing);
    REXGPU_INFO(
        "fable2-renderer: Vulkan {} device '{}' (api {}.{}.{}, driver 0x{:x}, vendor 0x{:04x}) "
        "queues g{} c{} t{} | dynamic-rendering {} timeline {} sync2 {} spirv14 {} "
        "robustness2 {} mem-budget {} | presentation {}",
        ctx->caps_.core13 ? "1.3" : (ctx->caps_.core12 ? "1.2" : "1.0/1.1"),
        ctx->caps_.deviceName,
        VK_VERSION_MAJOR(ctx->caps_.deviceApiVersion),
        VK_VERSION_MINOR(ctx->caps_.deviceApiVersion),
        VK_VERSION_PATCH(ctx->caps_.deviceApiVersion), ctx->caps_.driverVersion,
        ctx->caps_.vendorId, ctx->caps_.graphicsQueueCount, ctx->caps_.computeQueueCount,
        ctx->caps_.transferFamily >= 0 ? 1 : 0, ctx->caps_.dynamicRendering,
        ctx->caps_.timelineSemaphore, ctx->caps_.sync2, ctx->caps_.spirv14,
        ctx->caps_.robustness2, ctx->caps_.memoryBudget, ctx->caps_.presentationSupported);
    if (!sufficient) {
      std::string m;
      for (size_t i = 0; i < missing.size(); ++i) {
        m += (i ? ", " : "") + missing[i];
      }
      REXGPU_ERROR("fable2-renderer: device lacks required capabilities: {} — falling back", m);
      return false;
    }
  }

  out = std::move(ctx);
  return true;
}

bool VkContext::CreateInstance(std::string& err) {
  auto& fn = fn_;
  auto& caps = caps_;

  // API version (cap the loader's version at what the renderer targets).
  uint32_t apiVersion = config_.apiVersion;
  if (fn.vkEnumerateInstanceVersion) {
    uint32_t loaderVersion = 0;
    fn.vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < apiVersion) {
      apiVersion = loaderVersion;
    }
  }
  caps.apiVersion = apiVersion;

  // Instance extensions.
  std::vector<VkExtensionProperties> available;
  {
    uint32_t count = 0;
    fn.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    available.resize(count);
    if (count) fn.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
  }
  std::vector<std::string> availableNames;
  availableNames.reserve(available.size());
  for (auto& e : available) availableNames.emplace_back(e.extensionName);

  std::vector<std::string> requested;
  requested.emplace_back("VK_EXT_debug_utils");
  requested.emplace_back("VK_KHR_portability_enumeration");
  if (config_.withPresentation) {
    requested.emplace_back("VK_KHR_surface");
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    requested.emplace_back("VK_KHR_win32_surface");
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    requested.emplace_back("VK_KHR_xcb_surface");
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    requested.emplace_back("VK_KHR_wayland_surface");
#endif
  }
  std::vector<const char*> enabledExts;
  for (auto& r : requested) {
    if (StringHas(availableNames, r.c_str())) {
      enabledExts.push_back(r.c_str());
      caps.instanceExtensions.push_back(r);
    }
  }
  if (config_.withPresentation && !StringHas(availableNames, "VK_KHR_surface")) {
    err = "Vulkan instance does not support VK_KHR_surface (presentation required)";
    return false;
  }

  // Layers.
  std::vector<VkLayerProperties> layers;
  {
    uint32_t count = 0;
    fn.vkEnumerateInstanceLayerProperties(&count, nullptr);
    layers.resize(count);
    if (count) fn.vkEnumerateInstanceLayerProperties(&count, layers.data());
  }
  std::vector<const char*> enabledLayers;
  if (config_.withValidation) {
    bool have = false;
    for (auto& l : layers) {
      if (std::string(l.layerName) == "VK_LAYER_KHRONOS_validation") {
        enabledLayers.push_back(l.layerName);
        have = true;
        break;
      }
    }
    if (!have) {
      REXGPU_WARN("fable2-renderer: VK_LAYER_KHRONOS_validation requested but not found");
    }
  }
  caps.validationEnabled = !enabledLayers.empty();

  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = config_.applicationName;
  appInfo.applicationVersion = 1;
  appInfo.pEngineName = "fable2-vulkan-renderer";
  appInfo.engineVersion = 1;
  appInfo.apiVersion = apiVersion;

  VkInstanceCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &appInfo;
  ici.enabledLayerCount = (uint32_t)enabledLayers.size();
  ici.ppEnabledLayerNames = enabledLayers.data();
  ici.enabledExtensionCount = (uint32_t)enabledExts.size();
  ici.ppEnabledExtensionNames = enabledExts.data();
  if (StringHas(caps.instanceExtensions, "VK_KHR_portability_enumeration")) {
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }

  VkResult res = fn.vkCreateInstance(&ici, nullptr, &instance_);
  if (res == VK_ERROR_LAYER_NOT_PRESENT || res == VK_ERROR_EXTENSION_NOT_PRESENT) {
    // Retry without layers.
    enabledLayers.clear();
    ici.enabledLayerCount = 0;
    ici.ppEnabledLayerNames = nullptr;
    caps.validationEnabled = false;
    res = fn.vkCreateInstance(&ici, nullptr, &instance_);
  }
  if (res != VK_SUCCESS) {
    err = std::string("vkCreateInstance failed (VkResult=") + std::to_string(res) + ")";
    return false;
  }
  if (!fn.LoadInstance(instance_, err)) {
    return false;
  }

  // Debug utils messenger (always, if available) so Vulkan diagnostics reach
  // the renderer's log.
  if (StringHas(caps.instanceExtensions, "VK_EXT_debug_utils")) {
    fn.vkCreateDebugUtilsMessengerEXT =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            fn.vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    fn.vkDestroyDebugUtilsMessengerEXT =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            fn.vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn.vkCreateDebugUtilsMessengerEXT) {
      VkDebugUtilsMessengerCreateInfoEXT dmi = {};
      dmi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      dmi.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
      dmi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      dmi.pfnUserCallback = +[](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                VkDebugUtilsMessageTypeFlagsEXT,
                                const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
        const char* sev = "INFO";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) sev = "ERROR";
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) sev = "WARN";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) sev = "VERBOSE";
        if (data && data->pMessage) {
          REXGPU_DEBUG("[fable2-renderer vulkan/{}] {}", sev, data->pMessage);
        }
        return VK_FALSE;
      };
      fn.vkCreateDebugUtilsMessengerEXT(instance_, &dmi, nullptr, &debugMessenger_);
    }
  }

  REXGPU_INFO("fable2-renderer: Vulkan instance created (api {}.{}.{}, validation {})",
              VK_VERSION_MAJOR(caps.apiVersion), VK_VERSION_MINOR(caps.apiVersion),
              VK_VERSION_PATCH(caps.apiVersion), caps.validationEnabled ? "on" : "off");
  return true;
}

bool VkContext::SelectDevice(std::string& err) {
  auto& fn = fn_;
  auto& caps = caps_;

  std::vector<VkPhysicalDevice> devices;
  {
    uint32_t count = 0;
    fn.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
      err = "no Vulkan physical devices found";
      return false;
    }
    devices.resize(count);
    fn.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  }

  // Score each device; prefer a discrete GPU with a recent Vulkan version.
  struct Candidate {
    VkPhysicalDevice device;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    uint32_t score = 0;
  };
  std::vector<Candidate> cands;
  cands.reserve(devices.size());
  for (auto d : devices) {
    Candidate c;
    c.device = d;
    fn.vkGetPhysicalDeviceProperties(d, &c.props);
    fn.vkGetPhysicalDeviceFeatures(d, &c.features);

    uint32_t s = 0;
    s += c.props.apiVersion;  // prefer newer Vulkan
    switch (c.props.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: s += 1000; break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: s += 500; break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: s += 700; break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU: s -= 10000; break;
      default: break;
    }
    c.score = s;
    REXGPU_INFO("fable2-renderer: candidate device '{}' type={} api {}.{}.{} (score {})",
                c.props.deviceName, (int)c.props.deviceType, VK_VERSION_MAJOR(c.props.apiVersion),
                VK_VERSION_MINOR(c.props.apiVersion), VK_VERSION_PATCH(c.props.apiVersion), s);
    cands.push_back(std::move(c));
  }

  // Pick by forced name, else highest score.
  const Candidate* chosen = nullptr;
  if (!config_.forcedDeviceName.empty()) {
    for (auto& c : cands) {
      if (c.props.deviceName == config_.forcedDeviceName) {
        chosen = &c;
        break;
      }
    }
    if (!chosen) {
      err = "forced device '" + config_.forcedDeviceName + "' not found";
      return false;
    }
  } else {
    for (auto& c : cands) {
      if (!chosen || c.score > chosen->score) chosen = &c;
    }
  }

  // Populate capabilities from the chosen device.
  caps.deviceApiVersion = chosen->props.apiVersion;
  caps.driverVersion = chosen->props.driverVersion;
  caps.vendorId = chosen->props.vendorID;
  caps.deviceId = chosen->props.deviceID;
  caps.deviceName = chosen->props.deviceName;
  caps.core11 = caps.deviceApiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0);
  caps.core12 = caps.deviceApiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0);
  caps.core13 = caps.deviceApiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0);
  caps.features = chosen->features;
  caps.maxImageDimension2D = chosen->props.limits.maxImageDimension2D;
  caps.maxBoundDescriptorSets = chosen->props.limits.maxBoundDescriptorSets;
  caps.maxColorAttachments = chosen->props.limits.maxColorAttachments;
  caps.maxStorageBufferRange = chosen->props.limits.maxStorageBufferRange;
  caps.maxUniformBufferRange = chosen->props.limits.maxUniformBufferRange;
  caps.minUniformBufferOffsetAlignment = chosen->props.limits.minUniformBufferOffsetAlignment;
  caps.minStorageBufferOffsetAlignment =
      chosen->props.limits.minStorageBufferOffsetAlignment;
  caps.nonCoherentAtomSize = chosen->props.limits.nonCoherentAtomSize;

  // Device extension availability (to know what we can request).
  PFN_vkEnumerateDeviceExtensionProperties eprops =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          fn.vkGetInstanceProcAddr(instance_, "vkEnumerateDeviceExtensionProperties"));
  std::vector<std::string> devExtNames;
  if (eprops) {
    uint32_t n = 0;
    eprops(chosen->device, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> devExts(n);
    if (n) eprops(chosen->device, nullptr, &n, devExts.data());
    for (auto& e : devExts) devExtNames.emplace_back(e.extensionName);
  }
  const auto hasDevExt = [&](const char* n) { return StringHas(devExtNames, n); };

  // Feature detection (a feature is available if it is core at the device's
  // API level or present as an extension).
  caps.timelineSemaphore = caps.core12 || hasDevExt("VK_KHR_timeline_semaphore");
  caps.spirv14 = caps.deviceApiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0) ||
                 hasDevExt("VK_KHR_spirv_1_4");
  caps.dynamicRendering =
      caps.core13 || hasDevExt("VK_KHR_dynamic_rendering");
  caps.maintenance4 = caps.core13 || hasDevExt("VK_KHR_maintenance4");
  caps.memoryBudget = hasDevExt("VK_EXT_memory_budget");
  caps.dedicatedAllocation =
      caps.core11 || hasDevExt("VK_KHR_dedicated_allocation");
  caps.robustness2 = caps.core13 || hasDevExt("VK_EXT_robustness2");
  // 1.2 features are not in the base VkPhysicalDeviceFeatures; on a Vulkan 1.2+
  // discrete device they are effectively always present, and vkCreateDevice will
  // fail gracefully (=> fallback) if a requested feature is actually absent.
  caps.shaderFloat16 = caps.core12;
  caps.shaderInt16 = caps.core12;
  caps.bufferDeviceAddress = caps.core12;
  caps.descriptorIndexing = caps.core12;
  caps.sync2 = caps.core12;  // VK 1.2 core
  caps.shaderStencilExport = hasDevExt("VK_EXT_shader_stencil_export");
  caps.fragmentShaderInterlock = hasDevExt("VK_EXT_fragment_shader_interlock");
  caps.nonSeamlessCubeMap = hasDevExt("VK_EXT_non_seamless_cube_map");
  caps.customBorderColor = hasDevExt("VK_EXT_custom_border_color");
  caps.ycbcrConversion = hasDevExt("VK_KHR_sampler_ycbcr_conversion");

  // Queue families.
  std::vector<VkQueueFamilyProperties> qfams;
  {
    uint32_t count = 0;
    fn.vkGetPhysicalDeviceQueueFamilyProperties(chosen->device, &count, nullptr);
    qfams.resize(count);
    fn.vkGetPhysicalDeviceQueueFamilyProperties(chosen->device, &count, qfams.data());
  }
  int gfx = -1, comp = -1, xfer = -1;
  for (size_t i = 0; i < qfams.size(); ++i) {
    const VkQueueFlags f = qfams[i].queueFlags;
    if (gfx < 0 && (f & VK_QUEUE_GRAPHICS_BIT)) gfx = (int)i;
    if (comp < 0 && (f & VK_QUEUE_COMPUTE_BIT) && (f & VK_QUEUE_GRAPHICS_BIT)) {
      comp = (int)i;  // a graphics family can always run compute
    }
    if (xfer < 0 && (f & VK_QUEUE_TRANSFER_BIT) &&
        (gfx < 0 || (int)i != gfx)) {
      xfer = (int)i;  // a distinct transfer family, if one exists
    }
  }
  if (gfx < 0) {
    err = "chosen device has no graphics queue family";
    return false;
  }
  if (comp < 0) comp = gfx;
  if (xfer < 0) xfer = config_.separateTransferQueue ? -1 : gfx;
  caps.graphicsFamily = gfx;
  caps.computeFamily = comp;
  caps.transferFamily = (xfer == gfx) ? -1 : xfer;
  caps.graphicsQueueCount = std::max(1, config_.graphicsQueueCount);
  caps.computeQueueCount = std::max(1, config_.computeQueueCount);

  // Memory types.
  {
    fn.vkGetPhysicalDeviceMemoryProperties(chosen->device, &memoryProperties_);
    caps.memoryTypeCount = memoryProperties_.memoryTypeCount;
  }

  physicalDevice_ = chosen->device;
  REXGPU_INFO("fable2-renderer: selected device '{}'", caps.deviceName);
  return true;
}

bool VkContext::CreateDevice(std::string& err) {
  auto& fn = fn_;
  auto& caps = caps_;

  // Determine the single family we pull queues from (graphics family).
  gfxFamily_ = (uint32_t)caps.graphicsFamily;
  computeFamily_ = (uint32_t)caps.computeFamily;
  transferFamily_ = caps.transferFamily >= 0 ? (uint32_t)caps.transferFamily : gfxFamily_;

  // Queues: graphics + compute (+ a distinct transfer family if selected).
  const uint32_t sameFamilyQueues = (uint32_t)(caps.graphicsQueueCount + caps.computeQueueCount);
  std::vector<VkDeviceQueueCreateInfo> qcis;
  std::vector<float> priorities(sameFamilyQueues, 1.0f);
  {
    VkDeviceQueueCreateInfo q = {};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = gfxFamily_;
    q.queueCount = sameFamilyQueues;
    q.pQueuePriorities = priorities.data();
    qcis.push_back(q);
  }
  if (transferFamily_ != gfxFamily_) {
    VkDeviceQueueCreateInfo q = {};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = transferFamily_;
    q.queueCount = 1;
    const float tp = 1.0f;
    q.pQueuePriorities = &tp;
    qcis.push_back(q);
  }

  // Device extensions.
  std::vector<VkExtensionProperties> devExts;
  PFN_vkEnumerateDeviceExtensionProperties eprops =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          fn.vkGetInstanceProcAddr(instance_, "vkEnumerateDeviceExtensionProperties"));
  if (eprops) {
    uint32_t n = 0;
    eprops(physicalDevice_, nullptr, &n, nullptr);
    devExts.resize(n);
    if (n) eprops(physicalDevice_, nullptr, &n, devExts.data());
  }
  std::vector<std::string> devExtNames;
  for (auto& e : devExts) devExtNames.emplace_back(e.extensionName);

  std::vector<std::string> requested;
  const auto req = [&](const char* name, const char* promotedAt /*"1.2" or ""*/) {
    // Skip if it is core at the requested API level and that level is met.
    if (std::string(promotedAt) == "1.2" && caps.core12) return;
    if (std::string(promotedAt) == "1.3" && caps.core13) return;
    requested.emplace_back(name);
  };
  if (config_.withPresentation) req("VK_KHR_swapchain", "");
  // VMA requirements.
  req("VK_KHR_dedicated_allocation", "1.1");
  req("VK_KHR_get_memory_requirements2", "1.1");
  req("VK_KHR_bind_memory2", "1.1");
  req("VK_KHR_maintenance4", "1.3");
  req("VK_EXT_memory_budget", "");
  req("VK_KHR_spirv_1_4", "1.2");
  req("VK_KHR_dynamic_rendering", "1.3");
  req("VK_KHR_timeline_semaphore", "1.2");
  req("VK_EXT_shader_stencil_export", "");
  req("VK_EXT_robustness2", "");
  req("VK_EXT_fragment_shader_interlock", "");
  req("VK_EXT_non_seamless_cube_map", "");
  req("VK_EXT_custom_border_color", "");
  req("VK_KHR_sampler_ycbcr_conversion", "1.1");

  std::vector<const char*> enabled;
  for (auto& r : requested) {
    if (StringHas(devExtNames, r.c_str())) {
      enabled.push_back(r.c_str());
      caps.deviceExtensions.push_back(r);
    }
  }
  if (config_.withPresentation && !StringHas(caps.deviceExtensions, "VK_KHR_swapchain")) {
    err = "chosen device does not support VK_KHR_swapchain (presentation required)";
    return false;
  }

  // Vulkan 1.2/1.3/EXT features are enabled via the pNext chain below. The base
  // (1.0) features are left to the driver defaults: the overlay Vulkan layers
  // on this system (RivaTuner / graphics-hook) crash when a base-features node
  // or the pEnabledFeatures pointer is present in VkDeviceCreateInfo, and modern
  // discrete drivers enable the base features we rely on (robustBufferAccess,
  // imageCubeArray, samplerAnisotropy, textureCompressionBC) by default.
  VkPhysicalDeviceVulkan12Features f12 = {};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.timelineSemaphore = caps.timelineSemaphore ? VK_TRUE : VK_FALSE;
  f12.bufferDeviceAddress = caps.bufferDeviceAddress ? VK_TRUE : VK_FALSE;
  f12.descriptorIndexing = caps.descriptorIndexing ? VK_TRUE : VK_FALSE;
  f12.shaderFloat16 = caps.shaderFloat16 ? VK_TRUE : VK_FALSE;
  f12.shaderInt8 = VK_TRUE;
  f12.storageBuffer8BitAccess = VK_TRUE;
  f12.uniformAndStorageBuffer8BitAccess = VK_TRUE;

  VkPhysicalDeviceVulkan13Features f13 = {};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = caps.dynamicRendering ? VK_TRUE : VK_FALSE;
  f13.synchronization2 = caps.core12 ? VK_TRUE : VK_FALSE;
  f13.maintenance4 = caps.maintenance4 ? VK_TRUE : VK_FALSE;
  f13.robustImageAccess = caps.core13 ? VK_TRUE : VK_FALSE;

  // Optional EXT features.
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi = {};
  fsi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
  fsi.fragmentShaderSampleInterlock = caps.fragmentShaderInterlock ? VK_TRUE : VK_FALSE;
  fsi.fragmentShaderPixelInterlock = caps.fragmentShaderInterlock ? VK_TRUE : VK_FALSE;
  fsi.fragmentShaderShadingRateInterlock = caps.fragmentShaderInterlock ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT fns = {};
  fns.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT;
  fns.nonSeamlessCubeMap = caps.nonSeamlessCubeMap ? VK_TRUE : VK_FALSE;

  // Build the pNext chain from the 1.2/1.3/EXT feature structs only (each has a
  // valid sType and a pNext). Base features go via pEnabledFeatures above.
  struct Node {
    void* ptr;
    void** next;
  };
  std::vector<Node> nodes;
  if (caps.core12) nodes.push_back({&f12, &f12.pNext});
  if (caps.core13) nodes.push_back({&f13, &f13.pNext});
  if (StringHas(caps.deviceExtensions, "VK_EXT_fragment_shader_interlock"))
    nodes.push_back({&fsi, &fsi.pNext});
  if (StringHas(caps.deviceExtensions, "VK_EXT_non_seamless_cube_map"))
    nodes.push_back({&fns, &fns.pNext});
  for (size_t i = 0; i < nodes.size(); ++i) {
    *nodes[i].next = (i + 1 < nodes.size()) ? nodes[i + 1].ptr : nullptr;
  }

  VkDeviceCreateInfo dici = {};
  dici.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dici.pNext = nodes.empty() ? nullptr : nodes.front().ptr;
  dici.queueCreateInfoCount = (uint32_t)qcis.size();
  dici.pQueueCreateInfos = qcis.data();
  dici.enabledExtensionCount = (uint32_t)enabled.size();
  dici.ppEnabledExtensionNames = enabled.data();

  if (!fn.vkCreateDevice) {
    err = "vkCreateDevice function pointer is null";
    return false;
  }
  VkResult res = fn.vkCreateDevice(physicalDevice_, &dici, nullptr, &device_);
  if (res != VK_SUCCESS) {
    err = std::string("vkCreateDevice failed (VkResult=") + std::to_string(res) + ")";
    return false;
  }
  if (!fn.LoadDevice(device_, err)) {
    return false;
  }

  // Resolve queue handles.
  fn.vkGetDeviceQueue(device_, gfxFamily_, 0, &graphicsQueue_);
  fn.vkGetDeviceQueue(device_, computeFamily_,
                      (computeFamily_ == gfxFamily_) ? (uint32_t)caps.graphicsQueueCount : 0,
                      &computeQueue_);
  fn.vkGetDeviceQueue(device_, transferFamily_,
                      (transferFamily_ == gfxFamily_) ? sameFamilyQueues : 0, &transferQueue_);

  caps.presentationSupported = StringHas(caps.deviceExtensions, "VK_KHR_swapchain");
  REXGPU_INFO("fable2-renderer: Vulkan device created ({} extensions)", caps.deviceExtensions.size());
  return true;
}

bool VkContext::CreateVma(std::string& err) {
  VmaVulkanFunctions fv = {};
  fv.vkGetInstanceProcAddr = fn_.vkGetInstanceProcAddr;
  fv.vkGetDeviceProcAddr = fn_.vkGetDeviceProcAddr;
  fv.vkGetPhysicalDeviceProperties = fn_.vkGetPhysicalDeviceProperties;
  fv.vkGetPhysicalDeviceMemoryProperties = fn_.vkGetPhysicalDeviceMemoryProperties;
  fv.vkAllocateMemory = fn_.vkAllocateMemory;
  fv.vkFreeMemory = fn_.vkFreeMemory;
  fv.vkMapMemory = fn_.vkMapMemory;
  fv.vkUnmapMemory = fn_.vkUnmapMemory;
  fv.vkFlushMappedMemoryRanges = fn_.vkFlushMappedMemoryRanges;
  fv.vkInvalidateMappedMemoryRanges = fn_.vkInvalidateMappedMemoryRanges;
  fv.vkBindBufferMemory = fn_.vkBindBufferMemory;
  fv.vkBindImageMemory = fn_.vkBindImageMemory;
  fv.vkGetBufferMemoryRequirements = fn_.vkGetBufferMemoryRequirements;
  fv.vkGetImageMemoryRequirements = fn_.vkGetImageMemoryRequirements;
  fv.vkCreateBuffer = fn_.vkCreateBuffer;
  fv.vkDestroyBuffer = fn_.vkDestroyBuffer;
  fv.vkCreateImage = fn_.vkCreateImage;
  fv.vkDestroyImage = fn_.vkDestroyImage;
  fv.vkCmdCopyBuffer = fn_.vkCmdCopyBuffer;
  if (StringHas(caps_.deviceExtensions, "VK_KHR_get_memory_requirements2")) {
    fv.vkGetBufferMemoryRequirements2KHR = fn_.vkGetBufferMemoryRequirements2;
    fv.vkGetImageMemoryRequirements2KHR = fn_.vkGetImageMemoryRequirements2;
    if (StringHas(caps_.deviceExtensions, "VK_KHR_dedicated_allocation")) {
      allocatorFlags_ |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    }
  }
  if (StringHas(caps_.deviceExtensions, "VK_KHR_bind_memory2")) {
    fv.vkBindBufferMemory2KHR = fn_.vkBindBufferMemory2;
    fv.vkBindImageMemory2KHR = fn_.vkBindImageMemory2;
    allocatorFlags_ |= VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT;
  }
  if (fn_.vkGetDeviceBufferMemoryRequirements) {
    fv.vkGetDeviceBufferMemoryRequirements = fn_.vkGetDeviceBufferMemoryRequirements;
    fv.vkGetDeviceImageMemoryRequirements = fn_.vkGetDeviceImageMemoryRequirements;
  }

  VmaAllocatorCreateInfo info = {};
  info.vulkanApiVersion = caps_.deviceApiVersion;
  info.physicalDevice = physicalDevice_;
  info.device = device_;
  info.instance = instance_;
  info.pVulkanFunctions = &fv;
  info.flags = allocatorFlags_;
  if (vmaCreateAllocator(&info, &vma_) != VK_SUCCESS) {
    err = "vmaCreateAllocator failed";
    return false;
  }
  REXGPU_INFO("fable2-renderer: VMA allocator created");
  return true;
}

bool VkContext::CreatePipelineCache(std::string& err) {
  pipelineCache_ = std::make_unique<PipelineCache>(fn_, device_);
  return true;
}

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------
bool VkContext::AllocateBuffer(VkBuffer* outBuffer, VmaAllocation* outAlloc, VkDeviceSize size,
                               VmaMemoryUsage usage, VkBufferUsageFlags usageFlags,
                               VkMemoryPropertyFlags memProps, void** hostPtr,
                               std::string& err) {
  const bool hostVisible = (usage == VMA_MEMORY_USAGE_CPU_ONLY ||
                            (memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
  auto& pool = hostVisible ? memory_.HostVisiblePool() : memory_.DeviceLocalPool();
  if (memory_.Budget() != 0 && memory_.TotalResidentBytes() + size > memory_.Budget()) {
    err = "memory budget would be exceeded by " + std::to_string(size) + " byte buffer";
    return false;
  }

  VkBufferCreateInfo bci = {};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size;
  bci.usage = usageFlags;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci = {};
  aci.usage = usage;
  aci.requiredFlags = memProps;
  if (hostVisible) {
    aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  }

  VkResult res = vmaCreateBuffer(vma_, &bci, &aci, outBuffer, outAlloc, nullptr);
  if (res != VK_SUCCESS) {
    err = "vmaCreateBuffer failed (VkResult=" + std::to_string(res) + ")";
    return false;
  }
  memory_.Allocate(pool.name, size);
  if (hostPtr) {
    *hostPtr = nullptr;
    vmaMapMemory(vma_, *outAlloc, hostPtr);
  }
  return true;
}

namespace {
bool IsHostVisible(const VkPhysicalDeviceMemoryProperties& mp, uint32_t memoryType) {
  if (memoryType >= mp.memoryTypeCount) return false;
  return (mp.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}
}  // namespace

void VkContext::FreeBuffer(VkBuffer buffer, VmaAllocation alloc) {
  if (!alloc) return;
  VmaAllocationInfo info;
  vmaGetAllocationInfo(vma_, alloc, &info);
  const bool hostVisible = IsHostVisible(memoryProperties_, info.memoryType);
  memory_.Free(hostVisible ? memory_.HostVisiblePool() : memory_.DeviceLocalPool(), info.size);
  vmaDestroyBuffer(vma_, buffer, alloc);
}

bool VkContext::AllocateImage(VkImage* outImage, VmaAllocation* outAlloc,
                              const VkImageCreateInfo& cbi, VmaMemoryUsage usage, const char* label,
                              std::string& err) {
  (void)label;
  auto& pool = memory_.DeviceLocalPool();
  if (memory_.Budget() != 0 &&
      memory_.TotalResidentBytes() > memory_.Budget()) {
    err = "memory budget exhausted before image allocation";
    return false;
  }
  VmaAllocationCreateInfo aci = {};
  aci.usage = usage;
  VkResult res = vmaCreateImage(vma_, &cbi, &aci, outImage, outAlloc, nullptr);
  if (res != VK_SUCCESS) {
    err = "vmaCreateImage failed (VkResult=" + std::to_string(res) + ")";
    return false;
  }
  // Rough residency estimate from allocation info.
  VmaAllocationInfo info;
  vmaGetAllocationInfo(vma_, *outAlloc, &info);
  memory_.Allocate(pool.name, info.size);
  return true;
}

void VkContext::FreeImage(VkImage image, VmaAllocation alloc) {
  if (!alloc) return;
  VmaAllocationInfo info;
  vmaGetAllocationInfo(vma_, alloc, &info);
  memory_.Free(memory_.DeviceLocalPool(), info.size);
  vmaDestroyImage(vma_, image, alloc);
}

// ---------------------------------------------------------------------------
// Pipeline cache (disk + memory)
// ---------------------------------------------------------------------------
bool VkContext::LoadPipelineCache(std::string& err) {
  if (!pipelineCache_) return false;
  return pipelineCache_->LoadFromDisk(config_.pipelineCachePath, err);
}

bool VkContext::SavePipelineCache(std::string& err) {
  if (!pipelineCache_) return false;
  return pipelineCache_->SaveToDisk(config_.pipelineCachePath, err);
}

void VkContext::StorePipelineKey(const std::string& key, const void* data, size_t size) {
  if (pipelineCache_) pipelineCache_->StoreEntry(key, data, size);
}

const std::vector<uint8_t>* VkContext::LookupPipelineKey(const std::string& key) {
  return pipelineCache_ ? pipelineCache_->LookupEntry(key) : nullptr;
}

PipelineCache::Stats VkContext::PipelineCacheStats() const {
  return pipelineCache_ ? pipelineCache_->GetStats() : PipelineCache::Stats{};
}

void VkContext::SetObjectName(VkBuffer buffer, const char* name) {
  if (!buffer || !fn_.vkSetDebugUtilsObjectNameEXT) return;
  VkDebugUtilsObjectNameInfoEXT ni = {};
  ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  ni.objectType = VK_OBJECT_TYPE_BUFFER;
  ni.objectHandle = (uint64_t)buffer;
  ni.pObjectName = name;
  fn_.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

void VkContext::SetObjectName(VkImage image, const char* name) {
  if (!image || !fn_.vkSetDebugUtilsObjectNameEXT) return;
  VkDebugUtilsObjectNameInfoEXT ni = {};
  ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  ni.objectType = VK_OBJECT_TYPE_IMAGE;
  ni.objectHandle = (uint64_t)image;
  ni.pObjectName = name;
  fn_.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

void VkContext::SetObjectName(VmaAllocation alloc, const char* name) {
  if (!fn_.vkSetDebugUtilsObjectNameEXT) return;
  VmaAllocationInfo info;
  vmaGetAllocationInfo(vma_, alloc, &info);
  const uint64_t handle = info.deviceMemory ? (uint64_t)info.deviceMemory : 0;
  if (!handle) return;
  VkDebugUtilsObjectNameInfoEXT ni = {};
  ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  ni.objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;
  ni.objectHandle = handle;
  ni.pObjectName = name;
  fn_.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

void VkContext::WaitIdle() {
  if (device_ && fn_.vkDeviceWaitIdle) {
    fn_.vkDeviceWaitIdle(device_);
  }
}

}  // namespace fable2::renderer::vulkan
