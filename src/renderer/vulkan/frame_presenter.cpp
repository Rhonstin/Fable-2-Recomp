#include "frame_presenter.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <vulkan/vulkan.h>

namespace fable2::renderer::vulkan {
namespace {

uint64_t SteadyNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

bool FramePresenter::Create(VkContext& ctx, VkSurfaceKHR surface, uint32_t width, uint32_t height,
                            const FramePresenterConfig& config, std::unique_ptr<FramePresenter>& out,
                            std::string& err) {
  out.reset();
  err.clear();
  auto presenter = std::unique_ptr<FramePresenter>(new FramePresenter());
  if (!presenter->Init(ctx, surface, width, height, config)) {
    err = "FramePresenter: failed to initialize swapchain/present context";
    return false;
  }
  out = std::move(presenter);
  return true;
}

FramePresenter::~FramePresenter() {
  if (ctx_) {
    ctx_->WaitIdle();
  }
  DestroySwapchain();
  DestroySlots();
}

bool FramePresenter::Init(VkContext& ctx, VkSurfaceKHR surface, uint32_t width, uint32_t height,
                          const FramePresenterConfig& config) {
  ctx_ = &ctx;
  surface_ = surface;
  config_ = config;
  width_ = width;
  height_ = height;
  // Clamp framesInFlight to something sane.
  if (config_.framesInFlight < 2) {
    config_.framesInFlight = 2;
  }
  if (config_.framesInFlight > 8) {
    config_.framesInFlight = 8;
  }
  slotCount_ = config_.framesInFlight;
  std::string buildErr;
  if (!BuildSwapchain(width, height, buildErr)) {
    return false;
  }
  CreateSlots();
  startClockNs_ = SteadyNs();
  haveStartClock_ = true;
  return true;
}

void FramePresenter::PickSurfaceFormat(const VkSurfaceCapabilitiesKHR& caps, uint32_t width,
                                       uint32_t height, VkSurfaceFormatKHR* outFormat,
                                       VkExtent2D* outExtent) const {
  Functions& fn = ctx_->functions();
  const VkPhysicalDevice dev = ctx_->physicalDevice();
  std::vector<VkSurfaceFormatKHR> formats;
  uint32_t formatCount = 0;
  if (fn.vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &formatCount, nullptr) == VK_SUCCESS &&
      formatCount > 0) {
    formats.resize(formatCount);
    if (fn.vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &formatCount, formats.data()) ==
            VK_INCOMPLETE ||
        formatCount == 0) {
      formats.clear();
    }
  }
  // Prefer non-sRGB B8G8R8A8, then sRGB, then any.
  VkSurfaceFormatKHR chosen{};
  auto find = [&](VkFormat f, VkColorSpaceKHR cs) -> const VkSurfaceFormatKHR* {
    for (const auto& sf : formats) {
      if (sf.format == f && sf.colorSpace == cs) {
        return &sf;
      }
    }
    return nullptr;
  };
  if (const VkSurfaceFormatKHR* sf = find(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
    chosen = *sf;
  } else if (const VkSurfaceFormatKHR* sf =
                 find(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
    chosen = *sf;
  } else if (!formats.empty()) {
    chosen = formats.front();
  }
  *outFormat = chosen;

  // Extent: if the surface allows arbitrary size, clamp the requested size;
  // otherwise use the surface's current extent.
  const VkExtent2D& cur = caps.currentExtent;
  if (cur.width == 0xFFFFFFFFu || cur.height == 0xFFFFFFFFu) {
    uint32_t w = std::clamp<uint32_t>(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    uint32_t h = std::clamp<uint32_t>(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    *outExtent = VkExtent2D{w, h};
  } else {
    *outExtent = cur;
  }
}

VkPresentModeKHR FramePresenter::SelectPresentMode() const {
  Functions& fn = ctx_->functions();
  const VkPhysicalDevice dev = ctx_->physicalDevice();
  std::vector<VkPresentModeKHR> modes;
  uint32_t modeCount = 0;
  if (fn.vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &modeCount, nullptr) == VK_SUCCESS &&
      modeCount > 0) {
    modes.resize(modeCount);
    if (fn.vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &modeCount, modes.data()) ==
        VK_INCOMPLETE ||
        modeCount == 0) {
      modes.clear();
    }
  }
  auto has = [&](VkPresentModeKHR m) {
    return std::find(modes.begin(), modes.end(), m) != modes.end();
  };
  if (config_.vsync) {
    // Strict vsync by default; FIFO_RELAXED is an opt-in "cap but tear on slow
    // frames" mode.
    if (config_.allowFifoRelaxed && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
      return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }
  // Uncapped: prefer tearing (IMMEDIATE) > drop (MAILBOX) > FIFO_RELAXED > FIFO.
  if (config_.allowImmediate && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
  }
  if (config_.allowMailbox && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
    return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  if (config_.allowFifoRelaxed && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
    return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

bool FramePresenter::BuildSwapchain(uint32_t width, uint32_t height, std::string& err) {
  err.clear();
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  const VkPhysicalDevice dev = ctx_->physicalDevice();

  VkSurfaceCapabilitiesKHR caps{};
  if (fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface_, &caps) != VK_SUCCESS) {
    err = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
    return false;
  }
  VkSurfaceFormatKHR fmt{};
  VkExtent2D extent{};
  PickSurfaceFormat(caps, width, height, &fmt, &extent);
  format_ = fmt.format;
  extent_ = extent;
  width_ = extent.width;
  height_ = extent.height;
  presentMode_ = SelectPresentMode();

  uint32_t minImageCount = std::max<uint32_t>(slotCount_, caps.minImageCount);
  if (caps.maxImageCount > 0) {
    minImageCount = std::min(minImageCount, caps.maxImageCount);
  }

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = surface_;
  sci.minImageCount = minImageCount;
  sci.imageFormat = format_;
  sci.imageColorSpace = fmt.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.queueFamilyIndexCount = 0;
  sci.pQueueFamilyIndices = nullptr;
  sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                         ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                         : VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
  if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  } else {
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  }
  sci.presentMode = presentMode_;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;  // reuse images on resize (null on first create)

  VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
  if (fn.vkCreateSwapchainKHR(device, &sci, nullptr, &newSwapchain) != VK_SUCCESS) {
    err = "vkCreateSwapchainKHR failed";
    return false;
  }

  // Swapchain images + views.
  uint32_t imageCount = 0;
  if (fn.vkGetSwapchainImagesKHR(device, newSwapchain, &imageCount, nullptr) != VK_SUCCESS ||
      imageCount == 0) {
    fn.vkDestroySwapchainKHR(device, newSwapchain, nullptr);
    err = "vkGetSwapchainImagesKHR failed";
    return false;
  }
  images_.resize(imageCount);
  if (fn.vkGetSwapchainImagesKHR(device, newSwapchain, &imageCount, images_.data()) != VK_SUCCESS) {
    fn.vkDestroySwapchainKHR(device, newSwapchain, nullptr);
    err = "vkGetSwapchainImagesKHR (data) failed";
    return false;
  }
  // Destroy the old image views from a previous swapchain (on recreate) before
  // we overwrite them, so they don't leak. The old swapchain (and its images)
  // is only destroyed at the end of this function, so these views are still
  // valid here.
  for (VkImageView iv : imageViews_) {
    if (iv) {
      fn.vkDestroyImageView(device, iv, nullptr);
    }
  }
  imageViews_.assign(imageCount, VK_NULL_HANDLE);
  for (uint32_t i = 0; i < imageCount; ++i) {
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = images_[i];
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format_;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;
    if (fn.vkCreateImageView(device, &ivci, nullptr, &imageViews_[i]) != VK_SUCCESS) {
      err = "vkCreateImageView failed";
      // Clean up the partial state.
      for (uint32_t j = 0; j < i; ++j) {
        fn.vkDestroyImageView(device, imageViews_[j], nullptr);
      }
      fn.vkDestroySwapchainKHR(device, newSwapchain, nullptr);
      return false;
    }
  }

  // Render pass (single color attachment, UNDEFINED -> PRESENT_SRC).
  VkAttachmentDescription att{};
  att.format = format_;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = config_.renderLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;

  // Destroy the old render pass + framebuffers (if any) before creating new ones.
  if (renderPass_) {
    for (VkFramebuffer fb : framebuffers_) {
      if (fb) {
        fn.vkDestroyFramebuffer(device, fb, nullptr);
      }
    }
    framebuffers_.clear();
    fn.vkDestroyRenderPass(device, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
  }
  if (fn.vkCreateRenderPass(device, &rpci, nullptr, &renderPass_) != VK_SUCCESS) {
    err = "vkCreateRenderPass failed";
    // Destroy the new swapchain + views we just made.
    for (VkImageView iv : imageViews_) {
      if (iv) {
        fn.vkDestroyImageView(device, iv, nullptr);
      }
    }
    imageViews_.clear();
    images_.clear();
    fn.vkDestroySwapchainKHR(device, newSwapchain, nullptr);
    return false;
  }

  // Framebuffers (one per image).
  framebuffers_.assign(imageCount, VK_NULL_HANDLE);
  for (uint32_t i = 0; i < imageCount; ++i) {
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = renderPass_;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &imageViews_[i];
    fbci.width = extent.width;
    fbci.height = extent.height;
    fbci.layers = 1;
    if (fn.vkCreateFramebuffer(device, &fbci, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
      err = "vkCreateFramebuffer failed";
      for (uint32_t j = 0; j < i; ++j) {
        fn.vkDestroyFramebuffer(device, framebuffers_[j], nullptr);
      }
      framebuffers_.clear();
      fn.vkDestroyRenderPass(device, renderPass_, nullptr);
      renderPass_ = VK_NULL_HANDLE;
      for (VkImageView iv : imageViews_) {
        if (iv) {
          fn.vkDestroyImageView(device, iv, nullptr);
        }
      }
      imageViews_.clear();
      images_.clear();
      fn.vkDestroySwapchainKHR(device, newSwapchain, nullptr);
      return false;
    }
  }

  // Commit: the old swapchain (if any) is now retired.
  VkSwapchainKHR oldSwapchain = swapchain_;
  swapchain_ = newSwapchain;
  if (oldSwapchain != VK_NULL_HANDLE && oldSwapchain != newSwapchain) {
    fn.vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
  }
  return true;
}

void FramePresenter::DestroySwapchain() {
  if (!ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  for (VkFramebuffer fb : framebuffers_) {
    if (fb) {
      fn.vkDestroyFramebuffer(device, fb, nullptr);
    }
  }
  framebuffers_.clear();
  if (renderPass_) {
    fn.vkDestroyRenderPass(device, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
  }
  for (VkImageView iv : imageViews_) {
    if (iv) {
      fn.vkDestroyImageView(device, iv, nullptr);
    }
  }
  imageViews_.clear();
  images_.clear();
  if (swapchain_) {
    fn.vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

void FramePresenter::CreateSlots() {
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  const uint32_t family = (uint32_t)ctx_->caps().graphicsFamily;
  slots_.assign(slotCount_, Slot{});
  for (uint32_t i = 0; i < slotCount_; ++i) {
    Slot& s = slots_[i];
    // Fence created in the signaled state so the first BeginFrame wait returns
    // immediately.
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (fn.vkCreateFence(device, &fci, nullptr, &s.fence) != VK_SUCCESS) {
      continue;
    }
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    fn.vkCreateSemaphore(device, &sci, nullptr, &s.acquireSem);
    fn.vkCreateSemaphore(device, &sci, nullptr, &s.renderSem);
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    if (fn.vkCreateCommandPool(device, &cpci, nullptr, &s.pool) != VK_SUCCESS) {
      continue;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = s.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    fn.vkAllocateCommandBuffers(device, &cai, &s.cmd);
  }
}

void FramePresenter::DestroySlots() {
  if (!ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  for (Slot& s : slots_) {
    if (s.pool) {
      if (s.cmd) {
        VkCommandBuffer cb = s.cmd;
        fn.vkFreeCommandBuffers(device, s.pool, 1, &cb);
      }
      fn.vkDestroyCommandPool(device, s.pool, nullptr);
      s.pool = VK_NULL_HANDLE;
      s.cmd = VK_NULL_HANDLE;
    }
    if (s.acquireSem) {
      fn.vkDestroySemaphore(device, s.acquireSem, nullptr);
      s.acquireSem = VK_NULL_HANDLE;
    }
    if (s.renderSem) {
      fn.vkDestroySemaphore(device, s.renderSem, nullptr);
      s.renderSem = VK_NULL_HANDLE;
    }
    if (s.fence) {
      fn.vkDestroyFence(device, s.fence, nullptr);
      s.fence = VK_NULL_HANDLE;
    }
  }
  slots_.clear();
}

bool FramePresenter::BeginFrame() {
  if (slots_.empty() || !swapchain_) {
    return false;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  // Vblank pacing: if a target frame rate is set, sleep to maintain it. This
  // matches the host present cadence to the guest's vblank (e.g. 60 Hz on a
  // 144 Hz display) regardless of the swapchain's present mode.
  if (config_.targetFps > 0.0) {
    const uint64_t frameTimeNs = 1000000000ULL / uint64_t(config_.targetFps);
    const uint64_t nowNs = SteadyNs();
    if (haveLastFrameNs_ && nowNs < lastFrameNs_ + frameTimeNs) {
      const uint64_t sleepNs = lastFrameNs_ + frameTimeNs - nowNs;
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNs));
    }
    lastFrameNs_ = SteadyNs();
    haveLastFrameNs_ = true;
  }

  slot_ = uint32_t(frameCounter_ % slotCount_);
  Slot& s = slots_[slot_];

  // Wait for this slot's previous GPU work to finish (finite timeout). If the
  // GPU is behind by more than fenceTimeoutNs, drop this frame (do NOT reset the
  // fence — it is still pending and will be waited on again next lap).
  if (fn.vkWaitForFences(device, 1, &s.fence, VK_TRUE, config_.fenceTimeoutNs) != VK_SUCCESS) {
    stats_.droppedFence++;
    frameCounter_++;
    return false;
  }

  // The slot is free. Reset the fence + command buffer and begin recording.
  fn.vkResetFences(device, 1, &s.fence);
  fn.vkResetCommandBuffer(s.cmd, 0);
  VkCommandBufferBeginInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (fn.vkBeginCommandBuffer(s.cmd, &bci) != VK_SUCCESS) {
    frameCounter_++;
    return false;
  }

  // Acquire a swapchain image (finite timeout). On timeout / out-of-date, skip
  // the present (the slot still advances and the fence is still signaled via
  // EndFrame so the ring keeps moving).
  skipPresent_ = false;
  VkResult ar = fn.vkAcquireNextImageKHR(device, swapchain_, config_.acquireTimeoutNs, s.acquireSem,
                                         nullptr, &imageIndex_);
  switch (ar) {
    case VK_SUCCESS:
      break;
    case VK_SUBOPTIMAL_KHR:
      suboptimalPending_ = true;
      stats_.suboptimal++;
      break;
    case VK_TIMEOUT:
      skipPresent_ = true;
      stats_.skippedAcquire++;
      break;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      skipPresent_ = true;
      recreateNeeded_ = true;
      stats_.outOfDate++;
      break;
    default:
      skipPresent_ = true;
      break;
  }
  return true;
}

PresentStatus FramePresenter::EndFrame() {
  if (slots_.empty()) {
    return PresentStatus::kSkipped;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  const VkQueue queue = ctx_->graphicsQueue();
  Slot& s = slots_[slot_];

  if (fn.vkEndCommandBuffer(s.cmd) != VK_SUCCESS) {
    frameCounter_++;
    return PresentStatus::kSkipped;
  }

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (!skipPresent_) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &s.acquireSem;
    // NOTE: the vendored (legacy) VkSubmitInfo uses pWaitDstStageMask, not the
    // modern pWaitDstStageStages.
    si.pWaitDstStageMask = &waitStage;
  }
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cmd;
  if (!skipPresent_) {
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s.renderSem;
  }
  VkResult sr = fn.vkQueueSubmit(queue, 1, &si, s.fence);
  if (sr != VK_SUCCESS) {
    frameCounter_++;
    return PresentStatus::kDeviceLost;
  }

  PresentStatus status = PresentStatus::kOk;
  if (skipPresent_) {
    status = PresentStatus::kSkipped;
  } else {
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s.renderSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    pi.pResults = nullptr;
    VkResult pr = fn.vkQueuePresentKHR(queue, &pi);
    if (pr == VK_SUCCESS) {
      stats_.presented++;
      status = PresentStatus::kOk;
    } else if (pr == VK_SUBOPTIMAL_KHR) {
      stats_.presented++;
      stats_.suboptimal++;
      suboptimalPending_ = true;
      status = PresentStatus::kSuboptimal;
    } else if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR ||
               pr == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
      stats_.outOfDate++;
      recreateNeeded_ = true;
      status = PresentStatus::kOutOfDate;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
      status = PresentStatus::kDeviceLost;
    } else {
      // Unknown error — the image may be stuck acquired; force a recreate.
      recreateNeeded_ = true;
      status = PresentStatus::kOutOfDate;
    }
  }
  stats_.frames++;
  frameCounter_++;
  return status;
}

PresentStatus FramePresenter::PresentClear(float r, float g, float b, float a) {
  if (!BeginFrame()) {
    return PresentStatus::kSkipped;
  }
  if (!skipPresent_) {
    Functions& fn = ctx_->functions();
    VkCommandBuffer cmd = Cmd();
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = renderPass_;
    rpb.framebuffer = framebuffers_[imageIndex_];
    rpb.renderArea.offset = VkOffset2D{0, 0};
    rpb.renderArea.extent = extent_;
    VkClearValue cv{};
    cv.color.float32[0] = r;
    cv.color.float32[1] = g;
    cv.color.float32[2] = b;
    cv.color.float32[3] = a;
    rpb.clearValueCount = 1;
    rpb.pClearValues = &cv;
    fn.vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    fn.vkCmdEndRenderPass(cmd);
  }
  return EndFrame();
}

bool FramePresenter::Recreate(uint32_t width, uint32_t height, std::string& err) {
  if (width == 0 || height == 0) {
    err = "Recreate: zero size";
    return false;
  }
  // Await in-flight work before retiring the swapchain.
  if (ctx_) {
    ctx_->WaitIdle();
  }
  bool ok = BuildSwapchain(width, height, err);
  if (!ok) {
    return false;
  }
  width_ = width;
  height_ = height;
  recreateNeeded_ = false;
  suboptimalPending_ = false;
  return true;
}

void FramePresenter::ClearStatus() {
  recreateNeeded_ = false;
  suboptimalPending_ = false;
}

void FramePresenter::WaitIdle() {
  if (ctx_) {
    ctx_->WaitIdle();
  }
}

double FramePresenter::ElapsedSeconds() const {
  if (!haveStartClock_) {
    return 0.0;
  }
  return double(SteadyNs() - startClockNs_) / 1e9;
}

FramePresenterStats FramePresenter::Stats() const {
  FramePresenterStats s = stats_;
  double elapsed = ElapsedSeconds();
  if (elapsed > 0.0) {
    s.fps = double(s.presented) / elapsed;
  }
  return s;
}

}  // namespace fable2::renderer::vulkan
