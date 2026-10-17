// W10 — Per-frame arena allocator + state batcher.
//
// Cuts per-packet CPU cost by:
//   - Pre-allocating per-frame memory (no heap allocation per register write)
//   - Batching consecutive register writes into a "pending state" that is
//     committed only when a draw occurs (reduces vkCmdSet* calls)
//   - memcpy-only fast paths for vertex/index/constant buffer uploads
//   - Dirty-page tracking for texture re-uploads (skip unchanged pages)
//
// The arena resets each frame (BeginFrame/EndFrame). All per-frame GPU data
// (vertex buffers, index buffers, constant buffers) is staged into the arena
// and uploaded in one batch at the end of the frame.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Per-frame arena: a single large host-visible buffer that serves as the
// upload target for all per-frame GPU data (vertex, index, constants).
// Resets each frame; GPU data is uploaded in one batch.
class PerFrameArena {
 public:
  PerFrameArena() = default;
  ~PerFrameArena() { Shutdown(); }

  // Initialize the arena with a given capacity. Grows as needed.
  bool Init(VkContext* ctx, uint32_t initialCapacity = 256 * 1024);
  void Shutdown();

  // Reset the arena for a new frame (cursor back to 0).
  void BeginFrame() {
    cursor_ = 0;
    frameUploads_ = 0;
    frameBytes_ = 0;
  }

  // Allocate `size` bytes from the arena, returning a pointer + offset.
  // Aligns to `align` bytes (default 256 for Vulkan UBO).
  struct Alloc {
    void* ptr = nullptr;
    uint64_t offset = 0;
    uint32_t size = 0;
  };
  Alloc AllocBytes(uint32_t size, uint32_t align = 256);

  // Stage a memcpy from CPU data into the arena (fast path: no per-byte work).
  Alloc StageData(const void* src, uint32_t size, uint32_t align = 256);

  // Flush: upload all staged data to the GPU buffer in one vkCmdCopyBuffer.
  void Flush(VkCommandBuffer cmd);

  // Accessors.
  VkBuffer buffer() const { return buffer_; }
  uint32_t cursor() const { return cursor_; }
  uint32_t capacity() const { return capacity_; }
  uint64_t frameUploads() const { return frameUploads_; }
  uint64_t frameBytes() const { return frameBytes_; }
  uint64_t totalAllocations() const { return totalAllocs_; }
  uint64_t totalBytes() const { return totalBytes_; }

 private:
  bool EnsureCapacity(uint32_t needed);

  VkContext* ctx_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VmaAllocation alloc_ = VK_NULL_HANDLE;
  void* mapped_ = nullptr;
  uint32_t capacity_ = 0;
  uint32_t cursor_ = 0;
  uint64_t frameUploads_ = 0;
  uint64_t frameBytes_ = 0;
  uint64_t totalAllocs_ = 0;
  uint64_t totalBytes_ = 0;
};

// State batcher: accumulates register writes and commits them as a batch
// when a draw occurs. Reduces vkCmdSet* calls by only updating state that
// has actually changed since the last commit.
class StateBatcher {
 public:
  struct PendingState {
    // Viewport + scissor.
    float viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;
    float minDepth = 0, maxDepth = 1;
    uint32_t scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;
    bool scissorEnabled = false;
    bool viewportDirty = false;
    bool scissorDirty = false;

    // Blend.
    float blendR = 1, blendG = 1, blendB = 1, blendA = 1;
    bool blendDirty = false;

    // Depth bias.
    float depthBiasConstant = 0, depthBiasClamp = 0, depthBiasSlope = 0;
    bool depthBiasDirty = false;

    // Vertex/index buffer.
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    uint64_t vertexOffset = 0;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint64_t indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    bool vertexBufferDirty = false;
    bool indexBufferDirty = false;
  };

  // Mark a state component as dirty (from a register write).
  void MarkViewportDirty() { pending_.viewportDirty = true; }
  void MarkScissorDirty() { pending_.scissorDirty = true; }
  void MarkBlendDirty() { pending_.blendDirty = true; }
  void MarkDepthBiasDirty() { pending_.depthBiasDirty = true; }
  void MarkVertexBufferDirty() { pending_.vertexBufferDirty = true; }
  void MarkIndexBufferDirty() { pending_.indexBufferDirty = true; }

  // Commit the pending state to a command buffer (only dirty components).
  void Commit(VkCommandBuffer cmd);

  // Reset all dirty flags (after a commit or a full state rebuild).
  void ClearDirty() {
    pending_.viewportDirty = false;
    pending_.scissorDirty = false;
    pending_.blendDirty = false;
    pending_.depthBiasDirty = false;
    pending_.vertexBufferDirty = false;
    pending_.indexBufferDirty = false;
  }

  PendingState& pending() { return pending_; }
  const PendingState& pending() const { return pending_; }

  // Counters.
  uint64_t commits = 0;
  uint64_t dirtySkips = 0;  // times a commit was skipped (nothing dirty).

  void SetContext(VkContext* ctx) { ctx_ = ctx; }

 private:
  PendingState pending_;
  VkContext* ctx_ = nullptr;
};

// Dirty-page tracker for texture re-uploads. Tracks which pages of a texture
// have been modified since the last upload, so only dirty pages are re-uploaded.
class DirtyPageTracker {
 public:
  struct TextureDirtyState {
    // Bitmask of dirty pages (one bit per page).
    std::vector<uint64_t> dirtyBits;
    uint32_t pageCount = 0;
    uint32_t totalDirtyUploads = 0;
  };

  // Register a texture with a given number of pages.
  void Register(uint64_t guestAddress, uint32_t pageCount) {
    auto& state = textures_[guestAddress];
    state.pageCount = pageCount;
    state.dirtyBits.assign((pageCount + 63) / 64, 0);
    state.totalDirtyUploads = 0;
  }

  // Mark a page as dirty.
  void MarkDirty(uint64_t guestAddress, uint32_t pageIndex) {
    auto it = textures_.find(guestAddress);
    if (it == textures_.end()) return;
    auto& state = it->second;
    if (pageIndex < state.pageCount) {
      state.dirtyBits[pageIndex / 64] |= (1ULL << (pageIndex % 64));
    }
  }

  // Mark all pages as dirty (full re-upload).
  void MarkAllDirty(uint64_t guestAddress) {
    auto it = textures_.find(guestAddress);
    if (it == textures_.end()) return;
    auto& state = it->second;
    for (auto& bit : state.dirtyBits) bit = ~0ULL;
  }

  // Check if a page is dirty.
  bool IsDirty(uint64_t guestAddress, uint32_t pageIndex) const {
    auto it = textures_.find(guestAddress);
    if (it == textures_.end()) return true;  // Unknown = dirty.
    const auto& state = it->second;
    if (pageIndex >= state.pageCount) return false;
    return (state.dirtyBits[pageIndex / 64] & (1ULL << (pageIndex % 64))) != 0;
  }

  // Check if any page is dirty.
  bool AnyDirty(uint64_t guestAddress) const {
    auto it = textures_.find(guestAddress);
    if (it == textures_.end()) return true;
    const auto& state = it->second;
    for (const auto& bit : state.dirtyBits) {
      if (bit != 0) return true;
    }
    return false;
  }

  // Clear dirty bits after upload.
  void ClearDirty(uint64_t guestAddress) {
    auto it = textures_.find(guestAddress);
    if (it == textures_.end()) return;
    auto& state = it->second;
    for (auto& bit : state.dirtyBits) bit = 0;
    state.totalDirtyUploads++;
  }

  // Count of dirty uploads for a texture.
  uint32_t UploadCount(uint64_t guestAddress) const {
    auto it = textures_.find(guestAddress);
    return it != textures_.end() ? it->second.totalDirtyUploads : 0;
  }

  // Total bytes uploaded (for the profiler).
  uint64_t totalUploadBytes = 0;
  uint64_t totalUploadSkips = 0;  // pages skipped because clean.

 private:
  std::unordered_map<uint64_t, TextureDirtyState> textures_;
};

}  // namespace fable2::renderer::vulkan
