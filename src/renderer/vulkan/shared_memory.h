// W7 — Shared memory & blits/copy.
//
// Manages the Xenos SHARED memory region (512 MB, 2^29 bytes) as a
// host-visible, host-coherent VkBuffer. The CPU writes to it directly
// (via the mapped pointer) and the GPU reads from / writes to it via
// the VkBuffer handle. Handles COPY_TO_SHMEM, COPY_FROM_SHMEM,
// SHARED_FILL, and SHARED_SWAP operations.
//
// Also provides a thin BlitManager for immediate copy/blit commands:
// 2D blit, scale, fill (rect/clear), and buffer fill.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// The Xenos SHARED memory region is 2^29 = 512 MB.
static constexpr uint32_t kSharedMemSizeLog2 = 29;
static constexpr uint64_t kSharedMemSize = 1ULL << kSharedMemSizeLog2;

class SharedMemoryManager {
 public:
  // GPU usage for pipeline barriers (mirrors the SDK's Usage enum).
  enum class Usage {
    kRead,              // index buffer, vfetch, compute read, transfer source
    kGuestDrawReadWrite,  // index buffer, vfetch, memexport
    kComputeWrite,      // compute shader write
    kTransferDestination,  // blit/copy destination
  };

  bool Init(VkContext* ctx, std::string& err);
  void DestroyAll();

  // The GPU-visible buffer (for vkCmdCopyBuffer, vkCmdBindIndexBuffer, etc.).
  VkBuffer buffer() const { return buffer_; }
  // The CPU-visible mapped pointer (for direct CPU reads/writes).
  uint8_t* cpu() const { return map_; }

  // COPY_TO_SHMEM: CPU writes data into the shared memory at the given
  // guest offset.
  void CopyToShared(uint32_t guestOffset, const void* data, uint32_t size);
  // SHARED_FILL: CPU fills a region with a 32-bit pattern.
  void Fill(uint32_t guestOffset, uint32_t pattern, uint32_t size);
  // SHARED_SWAP: swap two regions in shared memory.
  void Swap(uint32_t offsetA, uint32_t offsetB, uint32_t size);
  // COPY_FROM_SHMEM: CPU reads data from the shared memory at the given
  // guest offset.
  void CopyFromShared(uint32_t guestOffset, void* outData, uint32_t size);

  // Pipeline barrier for the given GPU usage, ensuring correct ordering
  // between the CPU write and the subsequent GPU read (or vice versa).
  void Use(Usage usage, VkCommandBuffer cmd,
           std::pair<uint32_t, uint32_t> writtenRange = {});

  uint64_t copiesToShared = 0;
  uint64_t copiesFromShared = 0;
  uint64_t fills = 0;
  uint64_t swaps = 0;
  uint64_t barriers = 0;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stageMask,
                     VkAccessFlags& accessMask) const;

  VkContext* ctx_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VmaAllocation alloc_ = VK_NULL_HANDLE;
  uint8_t* map_ = nullptr;
};

// ---------------------------------------------------------------------------
// BlitManager — immediate copy/blit/clear commands.
// ---------------------------------------------------------------------------

class BlitManager {
 public:
  explicit BlitManager(VkContext* ctx) : ctx_(ctx) {}

  // 2D blit: copy a rectangle from srcImage to dstImage (same format, same size).
  void Blit2D(VkImage src, VkImage dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
              VkCommandBuffer cmd);
  // Scale blit: resize a rectangle from srcImage to dstImage.
  void BlitScale(VkImage src, VkImage dst,
                 uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH,
                 uint32_t dstX, uint32_t dstY, uint32_t dstW, uint32_t dstH,
              VkCommandBuffer cmd);
  // Fill an image rectangle with a color (vkCmdClearColorImage).
  void FillImage(VkImage image, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 float r, float g, float b, float a, VkCommandBuffer cmd);
  // Fill a buffer region with a 32-bit value (vkCmdFillBuffer).
  void FillBuffer(VkBuffer buffer, uint64_t offset, uint64_t size, uint32_t value,
                  VkCommandBuffer cmd);
  // Copy a buffer region (vkCmdCopyBuffer).
  void CopyBuffer(VkBuffer src, VkBuffer dst, uint64_t srcOffset, uint64_t dstOffset,
                  uint64_t size, VkCommandBuffer cmd);
  // Clear a depth/stencil image (vkCmdClearDepthStencilImage).
  void ClearDepthStencil(VkImage image, float depth, uint32_t stencil,
                         VkCommandBuffer cmd);
  // Clear render pass attachments (vkCmdClearAttachments) — use inside a render pass.
  void ClearAttachments(uint32_t attachmentIndex, float r, float g, float b, float a,
                        float depth, uint32_t stencil, uint32_t rectX, uint32_t rectY,
                        uint32_t rectW, uint32_t rectH, VkCommandBuffer cmd);

  uint64_t blits = 0;
  uint64_t scales = 0;
  uint64_t imageFills = 0;
  uint64_t depthClears = 0;
  uint64_t attachmentClears = 0;
  uint64_t bufferFills = 0;
  uint64_t bufferCopies = 0;

 private:
  VkContext* ctx_;
};

}  // namespace fable2::renderer::vulkan
