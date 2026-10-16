#pragma once

// W6: the guest render-target cache (framebuffer manager) on VkContext.
//
// Mirrors the SDK's VulkanRenderTargetCache conceptually (keyed guest render
// targets, color + depth image allocation, MSAA resolve, readback) but is
// self-contained and focused on the Fable 2 hot paths:
//   * keyed by (guest address, extent, color/depth format, msaa, color/depth
//     usage),
//   * reference-counted with LRU-ish eviction to a memory budget,
//   * a clean Xenos->Vulkan color/depth format table,
//   * MSAA 1x/2x/4x multisampled color + depth, resolved to the single-sample
//     guest target (the game's native MSAA path),
//   * guest RT contents preserved across frames (LOAD by default; the guest
//     clears explicitly), with the correct ATTACHMENT_OPTIMAL layout on first
//     touch,
//   * framebuffer objects keyed by (RT set, render pass) — reused, not
//     recreated (via the W3 RenderPassManager),
//   * render-to-texture + readback (CPU) of the single-sample guest surface
//     via a staging copy (the scratch-texture / hero-dog morph path).

#include <cstdint>
#include <functional>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// A Xenos render-target color format mapped to its host VkFormat.
struct XenosColorRTInfo {
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
};
// Maps a xenos::ColorRenderTargetFormat ordinal to a host color format.
// Returns false for formats with no host representation.
bool XenosToVulkanColorRT(uint32_t xenosFormat, XenosColorRTInfo& out);

// A Xenos render-target depth format mapped to its host VkFormat.
struct XenosDepthRTInfo {
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
};
// Maps a xenos::DepthRenderTargetFormat ordinal (kD24S8=0, kD24FS8=1) to a
// host depth/stencil format. Returns false for unknown formats.
bool XenosToVulkanDepthRT(uint32_t xenosFormat, XenosDepthRTInfo& out);

// Maps a xenos::MsaaSamples ordinal (k1X=0, k2X=1, k4X=2) to a Vk sample count.
VkSampleCountFlagBits MsaaToVulkan(uint32_t msaa);

// Sentinel for "no depth render target" in the key.
constexpr uint32_t kNoDepthFormat = 0xFFFFFFFFu;

// The guest key identifying a render target. Matches the W6 plan: (guest
// address, extent, format, msaa, color/depth usage).
struct GuestRenderTargetKey {
  uint64_t guestAddress = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colorFormat = 0;        // xenos::ColorRenderTargetFormat ordinal
  uint32_t depthFormat = kNoDepthFormat;  // xenos::DepthRenderTargetFormat ordinal
  uint32_t msaa = 0;               // xenos::MsaaSamples ordinal (k1X/k2X/k4X)
  bool hasDepth = false;           // depth/usage flag

  bool operator==(const GuestRenderTargetKey& o) const {
    return guestAddress == o.guestAddress && width == o.width && height == o.height &&
           colorFormat == o.colorFormat && depthFormat == o.depthFormat && msaa == o.msaa &&
           hasDepth == o.hasDepth;
  }
};

struct GuestRenderTargetKeyHash {
  size_t operator()(const GuestRenderTargetKey& k) const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) {
      h ^= v;
      h *= 1099511628211ULL;
    };
    mix(k.guestAddress);
    mix(k.width);
    mix(k.height);
    mix(k.colorFormat);
    mix(k.depthFormat);
    mix(k.msaa);
    mix(k.hasDepth ? 1u : 0u);
    return static_cast<size_t>(h);
  }
};

// A cached guest render target: an MSAA color image (+ optional depth), plus
// single-sample resolve images when MSAA > 1 (the guest-visible surfaces).
struct GuestRenderTarget {
  GuestRenderTargetKey key;
  // The render attachments (the MSAA color + depth images, `samples` samples).
  VkImage colorMsaa = VK_NULL_HANDLE;
  VkImage depthMsaa = VK_NULL_HANDLE;
  VmaAllocation colorMsaaAlloc = VK_NULL_HANDLE;
  VmaAllocation depthMsaaAlloc = VK_NULL_HANDLE;
  VkImageView colorMsaaView = VK_NULL_HANDLE;
  VkImageView depthMsaaView = VK_NULL_HANDLE;
  // The single-sample resolve targets (allocated only when samples > 1).
  VkImage colorResolve = VK_NULL_HANDLE;
  VkImage depthResolve = VK_NULL_HANDLE;
  VmaAllocation colorResolveAlloc = VK_NULL_HANDLE;
  VmaAllocation depthResolveAlloc = VK_NULL_HANDLE;
  VkImageView colorResolveView = VK_NULL_HANDLE;
  VkImageView depthResolveView = VK_NULL_HANDLE;

  VkFormat colorFormat = VK_FORMAT_UNDEFINED;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
  bool hasDepth = false;
  bool needsResolve = false;  // true when samples > 1 (a resolve is required)
  uint64_t bytes = 0;         // host image memory in bytes
  uint32_t refCount = 0;      // guest binding reference count
  uint64_t lastUsedFrame = 0; // LRU eviction ordering
  bool external = false;      // adopted external image; not freed on evict
  VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  bool Valid() const { return colorMsaa != VK_NULL_HANDLE && colorMsaaView != VK_NULL_HANDLE; }
};

class RenderTargetCache {
 public:
  RenderTargetCache() = default;
  ~RenderTargetCache();
  RenderTargetCache(const RenderTargetCache&) = delete;
  RenderTargetCache& operator=(const RenderTargetCache&) = delete;

  void Init(VkContext* ctx) {
    ctx_ = ctx;
  }

  // Get or create a render target for the given key (without a ref). Returns
  // the existing target if present, else allocates the images + views.
  GuestRenderTarget* CreateOrUpdate(const GuestRenderTargetKey& key, uint32_t frame);
  // Find an existing target (no create, no ref).
  GuestRenderTarget* Find(const GuestRenderTargetKey& key);
  // Add a guest binding reference. Returns the target (created if missing).
  GuestRenderTarget* Acquire(const GuestRenderTargetKey& key, uint32_t frame);
  // Drop a reference; evicts (frees images + views) when it hits 0 and is
  // eligible. Returns the number of bytes freed (0 if not evicted).
  uint64_t Release(GuestRenderTarget* rt, uint32_t frame);
  // Evict LRU targets down to the byte budget (only refcount-0 targets).
  uint32_t EvictToBudget(uint64_t budgetBytes);

  // The render attachments (the MSAA color + depth views) for the render pass.
  VkImageView ColorAttachment(const GuestRenderTarget* rt) const;
  VkImageView DepthAttachment(const GuestRenderTarget* rt) const;
  // The single-sample guest-visible surfaces (the resolve views when MSAA > 1,
  // else the 1x attachment views).
  VkImageView ColorSurface(const GuestRenderTarget* rt) const;
  VkImageView DepthSurface(const GuestRenderTarget* rt) const;

  // Resolve the MSAA color + depth to the single-sample targets (no-op for 1x).
  void Resolve(GuestRenderTarget* rt, VkCommandBuffer cmd);
  // Clear the single-sample color surface (a guest clear).
  void ClearColor(GuestRenderTarget* rt, float r, float g, float b, float a,
                  VkCommandBuffer cmd);
  // Clear the single-sample depth + stencil surface (a guest depth/stencil clear).
  void ClearDepthStencil(GuestRenderTarget* rt, float depth, uint32_t stencil,
                         VkCommandBuffer cmd);
  // Record a GPU-to-CPU copy of the single-sample color surface into a
  // host-visible buffer owned by the cache. The data is available after the
  // command buffer is submitted and the fence is waited on; call
  // ReadbackData() to get the CPU pointer.
  bool ReadbackColor(GuestRenderTarget* rt, uint32_t bytes, VkCommandBuffer cmd);
  // CPU pointer to the mapped readback buffer (valid after submit+wait).
  void* ReadbackData() { return readbackMap_; }

  // Adopt an external color image + view as a cached render target (the scratch
  // texture). external = true marks it as not owned by the cache (not freed).
  GuestRenderTarget* AdoptExternal(const GuestRenderTargetKey& key, VkImage colorImage,
                                   VkImageView colorView, VkFormat format, uint32_t frame);

  uint64_t TotalBytes() const { return totalBytes_; }
  size_t TargetCount() const { return targets_.size(); }
  const std::unordered_map<GuestRenderTargetKey, GuestRenderTarget, GuestRenderTargetKeyHash>&
  Targets() const {
    return targets_;
  }
  uint32_t evicted = 0;      // total targets evicted (freed)
  uint64_t resolves = 0;     // Resolve count
  uint64_t readbacks = 0;    // ReadbackColor count

  void DestroyAll();

 private:
  GuestRenderTarget* CreateTarget(const GuestRenderTargetKey& key, uint32_t frame);
  void FreeTarget(GuestRenderTarget* rt);
  // Allocate a color image (MSAA) and return the view.
  bool AllocateColor(GuestRenderTarget* rt, VkSampleCountFlagBits samples,
                     uint32_t mip, bool isResolve, std::string& err);
  // Allocate a depth image (MSAA) and return the view.
  bool AllocateDepth(GuestRenderTarget* rt, VkSampleCountFlagBits samples, uint32_t mip,
                     bool isResolve, std::string& err);
  VkImageView MakeView(VkImage image, VkFormat format, VkSampleCountFlagBits samples,
                       VkImageAspectFlags aspect) const;

  VkContext* ctx_ = nullptr;
  std::unordered_map<GuestRenderTargetKey, GuestRenderTarget, GuestRenderTargetKeyHash> targets_;
  uint64_t totalBytes_ = 0;
  // Persistent host-visible readback buffer (grown as needed).
  VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
  VmaAllocation readbackAlloc_ = VK_NULL_HANDLE;
  void* readbackMap_ = nullptr;
  uint32_t readbackCapacity_ = 0;
};

}  // namespace fable2::renderer::vulkan
