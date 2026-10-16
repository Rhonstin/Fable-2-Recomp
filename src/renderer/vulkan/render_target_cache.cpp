// W6: the guest render-target cache (framebuffer manager) on VkContext.
// See render_target_cache.h.

#include "render_target_cache.h"

#include <cstring>
#include <string>

#include <vk_mem_alloc.h>

namespace fable2::renderer::vulkan {

// --- Xenos color render-target format table. ---
bool XenosToVulkanColorRT(uint32_t xenosFormat, XenosColorRTInfo& out) {
  switch (xenosFormat) {
    case 0:  // k_8_8_8_8
      out.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
      return true;
    case 1:  // k_8_8_8_8_GAMMA (gamma handled by the load shader)
      out.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
      return true;
    case 2:  // k_2_10_10_10
      out.vkFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      return true;
    case 3:  // k_2_10_10_10_FLOAT (no packed-32 float in the vendored headers)
      out.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
      return true;
    case 4:  // k_16_16
      out.vkFormat = VK_FORMAT_R16G16_UNORM;
      return true;
    case 5:  // k_16_16_16_16
      out.vkFormat = VK_FORMAT_R16G16B16A16_UNORM;
      return true;
    case 6:  // k_16_16_FLOAT
      out.vkFormat = VK_FORMAT_R16G16_SFLOAT;
      return true;
    case 7:  // k_16_16_16_16_FLOAT
      out.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
      return true;
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      out.vkFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      return true;
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      out.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
      return true;
    case 14:  // k_32_FLOAT
      out.vkFormat = VK_FORMAT_R32_SFLOAT;
      return true;
    case 15:  // k_32_32_FLOAT
      out.vkFormat = VK_FORMAT_R32G32_SFLOAT;
      return true;
    default:
      return false;
  }
}

// --- Xenos depth render-target format table. ---
bool XenosToVulkanDepthRT(uint32_t xenosFormat, XenosDepthRTInfo& out) {
  switch (xenosFormat) {
    case 0:  // kD24S8
      out.vkFormat = VK_FORMAT_D24_UNORM_S8_UINT;
      return true;
    case 1:  // kD24FS8 (20e4 float depth)
      out.vkFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
      return true;
    default:
      return false;
  }
}

// --- MSAA mapping. ---
VkSampleCountFlagBits MsaaToVulkan(uint32_t msaa) {
  switch (msaa) {
    case 0:  // k1X
      return VK_SAMPLE_COUNT_1_BIT;
    case 1:  // k2X
      return VK_SAMPLE_COUNT_2_BIT;
    case 2:  // k4X
      return VK_SAMPLE_COUNT_4_BIT;
    default:
      return VK_SAMPLE_COUNT_1_BIT;
  }
}

RenderTargetCache::~RenderTargetCache() { DestroyAll(); }

GuestRenderTarget* RenderTargetCache::Find(const GuestRenderTargetKey& key) {
  auto it = targets_.find(key);
  return it != targets_.end() ? &it->second : nullptr;
}

GuestRenderTarget* RenderTargetCache::CreateOrUpdate(const GuestRenderTargetKey& key,
                                                     uint32_t frame) {
  if (Find(key)) {
    auto it = targets_.find(key);
    it->second.lastUsedFrame = frame;
    return &it->second;
  }
  return CreateTarget(key, frame);
}

GuestRenderTarget* RenderTargetCache::Acquire(const GuestRenderTargetKey& key, uint32_t frame) {
  GuestRenderTarget* rt = CreateOrUpdate(key, frame);
  if (rt) {
    rt->refCount++;
    rt->lastUsedFrame = frame;
  }
  return rt;
}

uint64_t RenderTargetCache::Release(GuestRenderTarget* rt, uint32_t frame) {
  if (!rt) {
    return 0;
  }
  if (rt->refCount > 0) {
    rt->refCount--;
  }
  rt->lastUsedFrame = frame;
  if (rt->refCount == 0 && !rt->external) {
    auto it = targets_.find(rt->key);
    if (it != targets_.end()) {
      const uint64_t freed = it->second.bytes;
      FreeTarget(&it->second);
      targets_.erase(it);
      evicted++;
      return freed;
    }
  }
  return 0;
}

uint32_t RenderTargetCache::EvictToBudget(uint64_t budgetBytes) {
  uint32_t evictedThisCall = 0;
  while (totalBytes_ > budgetBytes && !targets_.empty()) {
    GuestRenderTarget* lru = nullptr;
    for (auto& [k, t] : targets_) {
      if (t.refCount == 0 && !t.external &&
          (lru == nullptr || t.lastUsedFrame < lru->lastUsedFrame)) {
        lru = &t;
      }
    }
    if (!lru) {
      break;
    }
    FreeTarget(lru);
    targets_.erase(targets_.find(lru->key));
    evicted++;
    evictedThisCall++;
  }
  return evictedThisCall;
}

VkImageView RenderTargetCache::ColorAttachment(const GuestRenderTarget* rt) const {
  return rt ? rt->colorMsaaView : VK_NULL_HANDLE;
}
VkImageView RenderTargetCache::DepthAttachment(const GuestRenderTarget* rt) const {
  return rt && rt->hasDepth ? rt->depthMsaaView : VK_NULL_HANDLE;
}
VkImageView RenderTargetCache::ColorSurface(const GuestRenderTarget* rt) const {
  if (!rt) {
    return VK_NULL_HANDLE;
  }
  return (rt->needsResolve && rt->colorResolveView != VK_NULL_HANDLE) ? rt->colorResolveView
                                                                     : rt->colorMsaaView;
}
VkImageView RenderTargetCache::DepthSurface(const GuestRenderTarget* rt) const {
  if (!rt || !rt->hasDepth) {
    return VK_NULL_HANDLE;
  }
  return (rt->needsResolve && rt->depthResolveView != VK_NULL_HANDLE) ? rt->depthResolveView
                                                                      : rt->depthMsaaView;
}

void RenderTargetCache::Resolve(GuestRenderTarget* rt, VkCommandBuffer cmd) {
  if (!rt || !rt->needsResolve || !ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  // Color resolve (MSAA -> single sample).
  {
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.image = rt->colorMsaa;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkImageResolve region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {0, 0, 0};
    region.extent = {rt->width, rt->height, 1};
    fn.vkCmdResolveImage(cmd, rt->colorMsaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         rt->colorResolve, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  // Depth resolve (MSAA -> single sample) — both depth and stencil aspects.
  if (rt->hasDepth) {
    VkImageResolve region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = region.srcSubresource;
    region.dstOffset = {0, 0, 0};
    region.extent = {rt->width, rt->height, 1};
    fn.vkCmdResolveImage(cmd, rt->depthMsaa, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         rt->depthResolve, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  rt->colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (rt->hasDepth) {
    rt->depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  resolves++;
}

void RenderTargetCache::ClearColor(GuestRenderTarget* rt, float r, float g, float b, float a,
                                   VkCommandBuffer cmd) {
  if (!rt || !ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkImage target = rt->needsResolve ? rt->colorResolve : rt->colorMsaa;
  VkClearColorValue ccv{};
  ccv.float32[0] = r;
  ccv.float32[1] = g;
  ccv.float32[2] = b;
  ccv.float32[3] = a;
  const VkImageLayout layout = rt->needsResolve ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  fn.vkCmdClearColorImage(cmd, target, layout, &ccv, 1, &range);
}

void RenderTargetCache::ClearDepthStencil(GuestRenderTarget* rt, float depth, uint32_t stencil,
                                          VkCommandBuffer cmd) {
  if (!rt || !rt->hasDepth || !ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkImage target = rt->needsResolve ? rt->depthResolve : rt->depthMsaa;
  VkClearDepthStencilValue dsv{};
  dsv.depth = depth;
  dsv.stencil = stencil;
  const VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0,
                                      1, 0, 1};
  fn.vkCmdClearDepthStencilImage(cmd, target, layout, &dsv, 1, &range);
}

bool RenderTargetCache::ReadbackColor(GuestRenderTarget* rt, uint32_t bytes,
                                      VkCommandBuffer cmd) {
  if (!rt || !ctx_) {
    return false;
  }
  const VkDevice device = ctx_->device();
  Functions& fn = ctx_->functions();
  const VkImage target = rt->needsResolve ? rt->colorResolve : rt->colorMsaa;
  // Grow the persistent host-visible readback buffer if needed.
  if (bytes > readbackCapacity_) {
    if (readbackBuffer_ != VK_NULL_HANDLE) {
      ctx_->FreeBuffer(readbackBuffer_, readbackAlloc_);
      readbackBuffer_ = VK_NULL_HANDLE;
      readbackAlloc_ = VK_NULL_HANDLE;
      readbackMap_ = nullptr;
      readbackCapacity_ = 0;
    }
    readbackCapacity_ = bytes > 256 * 1024 ? bytes : 256 * 1024;
    std::string err;
    if (!ctx_->AllocateBuffer(&readbackBuffer_, &readbackAlloc_, readbackCapacity_,
                              VMA_MEMORY_USAGE_GPU_TO_CPU,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &readbackMap_, err)) {
      readbackBuffer_ = VK_NULL_HANDLE;
      readbackAlloc_ = VK_NULL_HANDLE;
      return false;
    }
  }
  // Barrier the color surface to TRANSFER_SRC, then copy to the host-visible buffer.
  {
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = rt->needsResolve ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.image = target;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rbr{};
    rbr.bufferOffset = 0;
    rbr.bufferRowLength = 0;
    rbr.bufferImageHeight = rt->height;
    rbr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rbr.imageOffset = {0, 0, 0};
    rbr.imageExtent = {rt->width, rt->height, 1};
    fn.vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_,
                              1, &rbr);
  }
  readbacks++;
  return true;
}

GuestRenderTarget* RenderTargetCache::AdoptExternal(const GuestRenderTargetKey& key,
                                                    VkImage colorImage, VkImageView colorView,
                                                    VkFormat format, uint32_t frame) {
  auto slot = targets_.emplace(key, GuestRenderTarget{}).first;
  GuestRenderTarget& rt = slot->second;
  rt.key = key;
  rt.colorMsaa = colorImage;
  rt.colorMsaaView = colorView;
  rt.colorResolveView = colorView;
  rt.colorFormat = format;
  rt.samples = VK_SAMPLE_COUNT_1_BIT;
  rt.width = key.width;
  rt.height = key.height;
  rt.needsResolve = false;
  rt.external = true;
  rt.refCount = 0;
  rt.lastUsedFrame = frame;
  return &rt;
}

void RenderTargetCache::FreeTarget(GuestRenderTarget* rt) {
  if (!rt || !ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  // External RTs (adopted scratch textures) are owned by the caller: the cache
  // does not free their images or views (the caller frees them).
  if (!rt->external) {
    auto destroyView = [&](VkImageView& v) {
      if (v != VK_NULL_HANDLE) {
        fn.vkDestroyImageView(device, v, nullptr);
        v = VK_NULL_HANDLE;
      }
    };
    destroyView(rt->colorMsaaView);
    destroyView(rt->colorResolveView);
    destroyView(rt->depthMsaaView);
    destroyView(rt->depthResolveView);
    if (rt->colorMsaa != VK_NULL_HANDLE) {
      ctx_->FreeImage(rt->colorMsaa, rt->colorMsaaAlloc);
      rt->colorMsaa = VK_NULL_HANDLE;
    }
    if (rt->colorResolve != VK_NULL_HANDLE) {
      ctx_->FreeImage(rt->colorResolve, rt->colorResolveAlloc);
      rt->colorResolve = VK_NULL_HANDLE;
    }
    if (rt->depthMsaa != VK_NULL_HANDLE) {
      ctx_->FreeImage(rt->depthMsaa, rt->depthMsaaAlloc);
      rt->depthMsaa = VK_NULL_HANDLE;
    }
    if (rt->depthResolve != VK_NULL_HANDLE) {
      ctx_->FreeImage(rt->depthResolve, rt->depthResolveAlloc);
      rt->depthResolve = VK_NULL_HANDLE;
    }
    if (rt->bytes > 0) {
      totalBytes_ = (totalBytes_ >= rt->bytes) ? (totalBytes_ - rt->bytes) : 0;
      rt->bytes = 0;
    }
  }
}

GuestRenderTarget* RenderTargetCache::CreateTarget(const GuestRenderTargetKey& key,
                                                   uint32_t frame) {
  if (!ctx_) {
    return nullptr;
  }
  XenosColorRTInfo cf;
  if (!XenosToVulkanColorRT(key.colorFormat, cf) || cf.vkFormat == VK_FORMAT_UNDEFINED) {
    return nullptr;
  }
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  if (key.hasDepth) {
    XenosDepthRTInfo df;
    if (!XenosToVulkanDepthRT(key.depthFormat, df) || df.vkFormat == VK_FORMAT_UNDEFINED) {
      return nullptr;
    }
    depthFormat = df.vkFormat;
  }
  // Clamp the MSAA sample count to what the device supports.
  VkSampleCountFlagBits samples = MsaaToVulkan(key.msaa);
  samples = ctx_->caps().ClampMsaa(samples);

  auto slot = targets_.emplace(key, GuestRenderTarget{}).first;
  GuestRenderTarget& rt = slot->second;
  rt.key = key;
  rt.colorFormat = cf.vkFormat;
  rt.depthFormat = depthFormat;
  rt.samples = samples;
  rt.width = key.width;
  rt.height = key.height;
  rt.hasDepth = key.hasDepth;
  rt.needsResolve = (samples > VK_SAMPLE_COUNT_1_BIT);
  rt.refCount = 0;
  rt.lastUsedFrame = frame;
  rt.bytes = 0;

  std::string err;
  if (!AllocateColor(&rt, samples, 0, false, err)) {
    targets_.erase(slot);
    return nullptr;
  }
  if (rt.needsResolve) {
    if (!AllocateColor(&rt, VK_SAMPLE_COUNT_1_BIT, 0, true, err)) {
      FreeTarget(&rt);
      targets_.erase(slot);
      return nullptr;
    }
  }
  if (key.hasDepth) {
    if (!AllocateDepth(&rt, samples, 0, false, err)) {
      FreeTarget(&rt);
      targets_.erase(slot);
      return nullptr;
    }
    if (rt.needsResolve) {
      if (!AllocateDepth(&rt, VK_SAMPLE_COUNT_1_BIT, 0, true, err)) {
        FreeTarget(&rt);
        targets_.erase(slot);
        return nullptr;
      }
    }
  }
  return &rt;
}

bool RenderTargetCache::AllocateColor(GuestRenderTarget* rt, VkSampleCountFlagBits samples,
                                      uint32_t mip, bool isResolve, std::string& err) {
  VkImageCreateInfo cbi{};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = rt->colorFormat;
  cbi.extent = {rt->width, rt->height, 1};
  cbi.mipLevels = 1;
  cbi.arrayLayers = 1;
  cbi.samples = samples;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!isResolve) {
    cbi.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage* outImage = isResolve ? &rt->colorResolve : &rt->colorMsaa;
  VmaAllocation* outAlloc = isResolve ? &rt->colorResolveAlloc : &rt->colorMsaaAlloc;
  if (!ctx_->AllocateImage(outImage, outAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY,
                           isResolve ? "guest rt resolve" : "guest rt msaa", err)) {
    return false;
  }
  VmaAllocationInfo ainfo{};
  vmaGetAllocationInfo(ctx_->vma(), *outAlloc, &ainfo);
  rt->bytes += ainfo.size;
  totalBytes_ += ainfo.size;

  VkImageView& outView = isResolve ? rt->colorResolveView : rt->colorMsaaView;
  outView = MakeView(*outImage, rt->colorFormat, samples, VK_IMAGE_ASPECT_COLOR_BIT);
  if (outView == VK_NULL_HANDLE) {
    ctx_->FreeImage(*outImage, *outAlloc);
    *outImage = VK_NULL_HANDLE;
    *outAlloc = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool RenderTargetCache::AllocateDepth(GuestRenderTarget* rt, VkSampleCountFlagBits samples,
                                      uint32_t mip, bool isResolve, std::string& err) {
  VkImageCreateInfo cbi{};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = rt->depthFormat;
  cbi.extent = {rt->width, rt->height, 1};
  cbi.mipLevels = 1;
  cbi.arrayLayers = 1;
  cbi.samples = samples;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage* outImage = isResolve ? &rt->depthResolve : &rt->depthMsaa;
  VmaAllocation* outAlloc = isResolve ? &rt->depthResolveAlloc : &rt->depthMsaaAlloc;
  if (!ctx_->AllocateImage(outImage, outAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY,
                           isResolve ? "guest rt depth resolve" : "guest rt depth", err)) {
    return false;
  }
  VmaAllocationInfo ainfo{};
  vmaGetAllocationInfo(ctx_->vma(), *outAlloc, &ainfo);
  rt->bytes += ainfo.size;
  totalBytes_ += ainfo.size;

  VkImageView& outView = isResolve ? rt->depthResolveView : rt->depthMsaaView;
  outView = MakeView(*outImage, rt->depthFormat, samples,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  if (outView == VK_NULL_HANDLE) {
    ctx_->FreeImage(*outImage, *outAlloc);
    *outImage = VK_NULL_HANDLE;
    *outAlloc = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

VkImageView RenderTargetCache::MakeView(VkImage image, VkFormat format,
                                        VkSampleCountFlagBits samples,
                                        VkImageAspectFlags aspect) const {
  if (image == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  VkImageViewCreateInfo vbi{};
  vbi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vbi.image = image;
  vbi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vbi.format = format;
  vbi.subresourceRange.aspectMask = aspect;
  vbi.subresourceRange.baseMipLevel = 0;
  vbi.subresourceRange.levelCount = 1;
  vbi.subresourceRange.layerCount = 1;
  VkImageView view;
  if (ctx_->functions().vkCreateImageView(ctx_->device(), &vbi, nullptr, &view) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return view;
}

void RenderTargetCache::DestroyAll() {
  if (!ctx_) {
    return;
  }
  for (auto& [key, rt] : targets_) {
    FreeTarget(&rt);
  }
  targets_.clear();
  totalBytes_ = 0;
  if (readbackBuffer_ != VK_NULL_HANDLE) {
    Functions& fn = ctx_->functions();
    ctx_->FreeBuffer(readbackBuffer_, readbackAlloc_);
    readbackBuffer_ = VK_NULL_HANDLE;
    readbackAlloc_ = VK_NULL_HANDLE;
    readbackMap_ = nullptr;
    readbackCapacity_ = 0;
  }
}

}  // namespace fable2::renderer::vulkan
