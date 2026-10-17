// W7 — Shared memory & blits/copy implementation.
#include "shared_memory.h"

#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

// ---------------------------------------------------------------------------
// SharedMemoryManager
// ---------------------------------------------------------------------------

bool SharedMemoryManager::Init(VkContext* ctx, std::string& err) {
  ctx_ = ctx;
  if (!ctx) {
    err = "no context";
    return false;
  }
  const VkDevice device = ctx->device();
  void* mapPtr = nullptr;
  if (!ctx->AllocateBuffer(&buffer_, &alloc_, kSharedMemSize,
                           VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &mapPtr, err)) {
    return false;
  }
  map_ = static_cast<uint8_t*>(mapPtr);
  return true;
}

void SharedMemoryManager::DestroyAll() {
  if (ctx_ && buffer_ != VK_NULL_HANDLE) {
    ctx_->FreeBuffer(buffer_, alloc_);
  }
  buffer_ = VK_NULL_HANDLE;
  alloc_ = VK_NULL_HANDLE;
  map_ = nullptr;
  ctx_ = nullptr;
}

void SharedMemoryManager::CopyToShared(uint32_t guestOffset, const void* data, uint32_t size) {
  if (map_ && guestOffset + size <= kSharedMemSize) {
    std::memcpy(map_ + guestOffset, data, size);
  }
  copiesToShared++;
}

void SharedMemoryManager::Fill(uint32_t guestOffset, uint32_t pattern, uint32_t size) {
  if (map_ && guestOffset + size <= kSharedMemSize) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(map_ + guestOffset);
    const uint32_t count = size / 4;
    for (uint32_t i = 0; i < count; ++i) {
      dst[i] = pattern;
    }
  }
  fills++;
}

void SharedMemoryManager::Swap(uint32_t offsetA, uint32_t offsetB, uint32_t size) {
  if (!map_ || offsetA + size > kSharedMemSize || offsetB + size > kSharedMemSize) {
    return;
  }
  uint8_t* a = map_ + offsetA;
  uint8_t* b = map_ + offsetB;
  // Use a 4 KB stack buffer for the swap (guest shared memory swaps are
  // typically small; large swaps would need a heap allocation).
  static constexpr uint32_t kChunk = 4096;
  uint8_t tmp[kChunk];
  uint32_t remaining = size;
  while (remaining > 0) {
    const uint32_t n = remaining < kChunk ? remaining : kChunk;
    std::memcpy(tmp, a, n);
    std::memcpy(a, b, n);
    std::memcpy(b, tmp, n);
    a += n;
    b += n;
    remaining -= n;
  }
  swaps++;
}

void SharedMemoryManager::CopyFromShared(uint32_t guestOffset, void* outData, uint32_t size) {
  if (map_ && guestOffset + size <= kSharedMemSize) {
    std::memcpy(outData, map_ + guestOffset, size);
  }
  copiesFromShared++;
}

void SharedMemoryManager::GetUsageMasks(Usage usage, VkPipelineStageFlags& stageMask,
                                        VkAccessFlags& accessMask) const {
  switch (usage) {
    case Usage::kRead:
      stageMask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT;
      accessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                   VK_ACCESS_UNIFORM_READ_BIT |
                   VK_ACCESS_SHADER_READ_BIT |
                   VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      stageMask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT;
      accessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                   VK_ACCESS_UNIFORM_READ_BIT |
                   VK_ACCESS_SHADER_READ_BIT |
                   VK_ACCESS_SHADER_WRITE_BIT |
                   VK_ACCESS_TRANSFER_READ_BIT |
                   VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    case Usage::kComputeWrite:
      stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      accessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    case Usage::kTransferDestination:
      stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      accessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
  }
}

void SharedMemoryManager::Use(Usage usage, VkCommandBuffer cmd,
                              std::pair<uint32_t, uint32_t> writtenRange) {
  if (!ctx_ || buffer_ == VK_NULL_HANDLE) {
    return;
  }
  Functions& f = ctx_->functions();
  VkPipelineStageFlags stageMask = 0;
  VkAccessFlags accessMask = 0;
  GetUsageMasks(usage, stageMask, accessMask);

  VkBufferMemoryBarrier bar{};
  bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  // Source: the CPU (HOST_WRITE) or a previous GPU operation.
  bar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                      VK_ACCESS_SHADER_WRITE_BIT;
  bar.dstAccessMask = accessMask;
  bar.buffer = buffer_;
  if (writtenRange.first != 0 || writtenRange.second != 0) {
    bar.offset = writtenRange.first;
    bar.size = writtenRange.second;
  } else {
    bar.offset = 0;
    bar.size = VK_WHOLE_SIZE;
  }
  // Use a broad source stage to cover any preceding GPU operation.
  const VkPipelineStageFlags srcStage =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT |
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  f.vkCmdPipelineBarrier(cmd, srcStage, stageMask, 0, 0, nullptr, 1, &bar, 0, nullptr);
  barriers++;
}

// ---------------------------------------------------------------------------
// BlitManager
// ---------------------------------------------------------------------------

void BlitManager::Blit2D(VkImage src, VkImage dst, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t h, VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
  region.extent = {w, h, 1};
  f.vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  blits++;
}

void BlitManager::BlitScale(VkImage src, VkImage dst,
                            uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH,
                            uint32_t dstX, uint32_t dstY, uint32_t dstW, uint32_t dstH,
                            VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {static_cast<int32_t>(srcX), static_cast<int32_t>(srcY), 0};
  region.srcOffsets[1] = {static_cast<int32_t>(srcX + srcW), static_cast<int32_t>(srcY + srcH), 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffsets[0] = {static_cast<int32_t>(dstX), static_cast<int32_t>(dstY), 0};
  region.dstOffsets[1] = {static_cast<int32_t>(dstX + dstW), static_cast<int32_t>(dstY + dstH), 1};
  f.vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
  scales++;
}

void BlitManager::FillImage(VkImage image, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            float r, float g, float b, float a, VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkClearColorValue clear{};
  clear.float32[0] = r;
  clear.float32[1] = g;
  clear.float32[2] = b;
  clear.float32[3] = a;
  // vkCmdClearColorImage clears the whole image; for a sub-rect, use a render
  // pass or a blit. For now, clear the whole image (the guest's fill is
  // typically full-surface).
  f.vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &sub);
  imageFills++;
}

void BlitManager::FillBuffer(VkBuffer buffer, uint64_t offset, uint64_t size, uint32_t value,
                             VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  f.vkCmdFillBuffer(cmd, buffer, offset, size, value);
  bufferFills++;
}

void BlitManager::CopyBuffer(VkBuffer src, VkBuffer dst, uint64_t srcOffset, uint64_t dstOffset,
                             uint64_t size, VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkBufferCopy copy{};
  copy.srcOffset = srcOffset;
  copy.dstOffset = dstOffset;
  copy.size = size;
  f.vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
  bufferCopies++;
}

void BlitManager::ClearDepthStencil(VkImage image, float depth, uint32_t stencil,
                                    VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
  VkClearDepthStencilValue dsv{};
  dsv.depth = depth;
  dsv.stencil = stencil;
  f.vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                                &dsv, 1, &sub);
  depthClears++;
}

void BlitManager::ClearAttachments(uint32_t attachmentIndex, float r, float g, float b, float a,
                                   float depth, uint32_t stencil, uint32_t rectX, uint32_t rectY,
                                   uint32_t rectW, uint32_t rectH, VkCommandBuffer cmd) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  VkClearAttachment att{};
  att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  att.colorAttachment = attachmentIndex;
  att.clearValue.color.float32[0] = r;
  att.clearValue.color.float32[1] = g;
  att.clearValue.color.float32[2] = b;
  att.clearValue.color.float32[3] = a;
  att.clearValue.depthStencil.depth = depth;
  att.clearValue.depthStencil.stencil = stencil;
  VkClearRect rect{};
  rect.rect.offset = {static_cast<int32_t>(rectX), static_cast<int32_t>(rectY)};
  rect.rect.extent = {rectW, rectH};
  rect.baseArrayLayer = 0;
  rect.layerCount = 1;
  f.vkCmdClearAttachments(cmd, 1, &att, 1, &rect);
  attachmentClears++;
}

}  // namespace fable2::renderer::vulkan
