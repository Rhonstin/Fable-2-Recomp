// W10 — Per-frame arena allocator + state batcher implementation.
#include "per_frame_arena.h"

#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

// --- PerFrameArena ---

bool PerFrameArena::Init(VkContext* ctx, uint32_t initialCapacity) {
  ctx_ = ctx;
  capacity_ = initialCapacity;
  std::string err;
  if (!ctx->AllocateBuffer(&buffer_, &alloc_, capacity_, VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &mapped_, err)) {
    return false;
  }
  cursor_ = 0;
  return true;
}

void PerFrameArena::Shutdown() {
  if (ctx_ && alloc_) {
    ctx_->FreeBuffer(buffer_, alloc_);
  }
  buffer_ = VK_NULL_HANDLE;
  alloc_ = VK_NULL_HANDLE;
  mapped_ = nullptr;
  capacity_ = 0;
  ctx_ = nullptr;
}

PerFrameArena::Alloc PerFrameArena::AllocBytes(uint32_t size, uint32_t align) {
  // Align the cursor.
  const uint32_t alignedCursor = (cursor_ + align - 1) & ~(align - 1);
  const uint32_t needed = alignedCursor + size;
  if (needed > capacity_) {
    if (!EnsureCapacity(needed)) {
      return {};
    }
  }
  Alloc a{static_cast<uint8_t*>(mapped_) + alignedCursor, alignedCursor, size};
  cursor_ = needed;
  totalAllocs_++;
  totalBytes_ += size;
  return a;
}

PerFrameArena::Alloc PerFrameArena::StageData(const void* src, uint32_t size, uint32_t align) {
  Alloc a = AllocBytes(size, align);
  if (a.ptr && src) {
    std::memcpy(a.ptr, src, size);
    frameUploads_++;
    frameBytes_ += size;
  }
  return a;
}

void PerFrameArena::Flush(VkCommandBuffer cmd) {
  if (cursor_ == 0 || !ctx_) return;
  // The arena is already host-visible and host-coherent, so the GPU can read
  // it directly without a separate vkCmdCopyBuffer. The Flush is a no-op for
  // host-coherent buffers; it exists for API completeness and for the
  // non-coherent fallback path.
  (void)cmd;
  // For non-coherent atoms, we'd need vkFlushMappedMemoryRanges here.
  // For host-coherent (RTX 3070), this is a no-op.
}

bool PerFrameArena::EnsureCapacity(uint32_t needed) {
  // Grow the arena by doubling (minimum 256KB).
  const uint32_t newCapacity = (needed < 256 * 1024) ? 256 * 1024 : (needed * 2);
  // Free the old buffer.
  if (alloc_) {
    ctx_->FreeBuffer(buffer_, alloc_);
  }
  capacity_ = newCapacity;
  std::string err;
  if (!ctx_->AllocateBuffer(&buffer_, &alloc_, capacity_, VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &mapped_, err)) {
    capacity_ = 0;
    return false;
  }
  // Reset cursor (all data is lost on grow).
  cursor_ = 0;
  return true;
}

// --- StateBatcher ---

void StateBatcher::Commit(VkCommandBuffer cmd) {
  if (!pending_.viewportDirty && !pending_.scissorDirty && !pending_.blendDirty &&
      !pending_.depthBiasDirty && !pending_.vertexBufferDirty && !pending_.indexBufferDirty) {
    dirtySkips++;
    return;
  }
  Functions& f = ctx_->functions();
  if (pending_.viewportDirty) {
    VkViewport vp{};
    vp.x = pending_.viewportX;
    vp.y = pending_.viewportY;
    vp.width = pending_.viewportW;
    vp.height = pending_.viewportH;
    vp.minDepth = pending_.minDepth;
    vp.maxDepth = pending_.maxDepth;
    f.vkCmdSetViewport(cmd, 0, 1, &vp);
    pending_.viewportDirty = false;
  }
  if (pending_.scissorDirty) {
    if (pending_.scissorEnabled) {
      VkRect2D sc{};
      sc.offset.x = (int32_t)pending_.scissorX;
      sc.offset.y = (int32_t)pending_.scissorY;
      sc.extent.width = pending_.scissorW;
      sc.extent.height = pending_.scissorH;
      f.vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    pending_.scissorDirty = false;
  }
  if (pending_.blendDirty) {
    const float blend[4] = {pending_.blendR, pending_.blendG, pending_.blendB, pending_.blendA};
    f.vkCmdSetBlendConstants(cmd, blend);
    pending_.blendDirty = false;
  }
  if (pending_.depthBiasDirty) {
    f.vkCmdSetDepthBias(cmd, pending_.depthBiasConstant, pending_.depthBiasClamp,
                        pending_.depthBiasSlope);
    pending_.depthBiasDirty = false;
  }
  if (pending_.vertexBufferDirty) {
    f.vkCmdBindVertexBuffers(cmd, 0, 1, &pending_.vertexBuffer, &pending_.vertexOffset);
    pending_.vertexBufferDirty = false;
  }
  if (pending_.indexBufferDirty) {
    f.vkCmdBindIndexBuffer(cmd, pending_.indexBuffer, pending_.indexOffset, pending_.indexType);
    pending_.indexBufferDirty = false;
  }
  commits++;
}

}  // namespace fable2::renderer::vulkan
