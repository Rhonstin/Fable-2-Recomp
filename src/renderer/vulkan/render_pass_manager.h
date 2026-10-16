// render_pass_manager.h — W3: render passes & subpass design.
//
// A small, cached set of VkRenderPass + VkFramebuffer objects built on
// VkContext. The guest frame is modeled as:
//
//   guest scene render -> [optional downscale/resolve] -> composite to swapchain
//
// Each stage is a render pass picked from this cache. The cache key captures
// exactly the fields that change a VkRenderPass (color format/samples/resolve,
// depth present/format/samples, load/store ops, and the Xenos framebuffer-fetch
// flag); everything else (blend, sample masks, depth writes, rasterization) is
// pipeline state, not render-pass state, so it does not belong in the key.
//
// Namespace: fable2::renderer::vulkan
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace fable2::renderer::vulkan {

class VkContext;

// ---------------------------------------------------------------------------
// Render-pass key.
//
// Two keys that differ in any field get distinct VkRenderPass handles. Load and
// store ops are part of the pass description (they are baked into the
// VkAttachmentDescription), so they are key fields here.
// ---------------------------------------------------------------------------
struct RenderPassKey {
  // --- color ---
  VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  VkSampleCountFlagBits colorSamples = VK_SAMPLE_COUNT_1_BIT;
  bool hasColorResolve = false;  // MSAA color resolved to a single-sample
                                 // attachment in the same pass (pResolve*).
  VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  VkAttachmentStoreOp colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE;

  // --- depth / stencil ---
  bool hasDepth = false;
  VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
  VkSampleCountFlagBits depthSamples = VK_SAMPLE_COUNT_1_BIT;
  VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_STORE;

  // --- framebuffer fetch (Xenos FETCH / memory-export) ---
  // When true, the color attachment is also declared as a subpass *input*
  // attachment in the same subpass (a feedback loop), letting the fragment
  // shader read the current framebuffer color. This emulates Xenos's per-pixel
  // framebuffer fetch without a separate pass.
  bool framebufferFetch = false;

  bool operator==(const RenderPassKey& o) const {
    return colorFormat == o.colorFormat && colorSamples == o.colorSamples &&
           hasColorResolve == o.hasColorResolve && colorLoadOp == o.colorLoadOp &&
           colorStoreOp == o.colorStoreOp && hasDepth == o.hasDepth &&
           depthFormat == o.depthFormat && depthSamples == o.depthSamples &&
           depthLoadOp == o.depthLoadOp && depthStoreOp == o.depthStoreOp &&
           framebufferFetch == o.framebufferFetch;
  }
};

struct RenderPassKeyHash {
  std::size_t operator()(const RenderPassKey& k) const {
    // FNV-1a over the packed fields.
    auto mix = [&k](std::size_t h, uint64_t v) {
      const uint64_t kF = 1099511628211ULL;
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= kF;
      }
      return h;
    };
    std::size_t h = std::hash<uint64_t>{}(1469598103934665603ULL);
    h = mix(h, uint64_t(k.colorFormat));
    h = mix(h, uint64_t(k.colorSamples));
    h = mix(h, uint64_t(k.hasColorResolve ? 1 : 0));
    h = mix(h, uint64_t(k.colorLoadOp));
    h = mix(h, uint64_t(k.colorStoreOp));
    h = mix(h, uint64_t(k.hasDepth ? 1 : 0));
    h = mix(h, uint64_t(k.depthFormat));
    h = mix(h, uint64_t(k.depthSamples));
    h = mix(h, uint64_t(k.depthLoadOp));
    h = mix(h, uint64_t(k.depthStoreOp));
    h = mix(h, uint64_t(k.framebufferFetch ? 1 : 0));
    return h;
  }
};

// The attachment indices of a resolved pass, so the caller knows which
// subpass attachment slot is color / resolve / depth.
struct PassLayout {
  VkRenderPass pass = VK_NULL_HANDLE;
  int colorAttachment = 0;
  int resolveAttachment = -1;  // -1 if no in-pass resolve
  int depthAttachment = -1;    // -1 if no depth
  uint32_t attachmentCount = 0;
};

// ---------------------------------------------------------------------------
// RenderPassManager.
// ---------------------------------------------------------------------------
class RenderPassManager {
 public:
  explicit RenderPassManager(VkContext* ctx);
  ~RenderPassManager();
  RenderPassManager(const RenderPassManager&) = delete;
  RenderPassManager& operator=(const RenderPassManager&) = delete;

  // Get (or create + cache) the render pass for `key`. On creation failure the
  // error is reported through ctx diagnostics and VK_NULL_HANDLE is returned.
  PassLayout GetPass(const RenderPassKey& key);

  // Create (or return a cached) framebuffer of `width`x`height` over `views`
  // for `pass`. The order of `views` must match the pass's attachment order
  // (color, [resolve], [depth]). Views must remain valid while the framebuffer
  // is alive. Returns VK_NULL_HANDLE on failure.
  VkFramebuffer GetFramebuffer(VkRenderPass pass, uint32_t width, uint32_t height,
                               const std::vector<VkImageView>& views);

  // Build a VkImageMemoryBarrier for a single-plane color/depth layout
  // transition (used for "correct layout transitions").
  static VkImageMemoryBarrier MakeBarrier(VkImage image, VkImageLayout oldLayout,
                                          VkImageLayout newLayout, VkAccessFlags srcAccess,
                                          VkAccessFlags dstAccess, VkImageAspectFlags aspects,
                                          uint32_t mipLevels = 1, uint32_t layers = 1);

  // Diagnostics.
  std::size_t passCount() const { return passCache_.size(); }
  std::size_t framebufferCount() const { return fbCache_.size(); }
  std::size_t passCreates() const { return passCreates_; }
  std::size_t passHits() const { return passHits_; }

  // Destroy every cached framebuffer + render pass (idempotent).
  void DestroyAll();

 private:
  struct FbKey {
    VkRenderPass pass = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint64_t> views;  // view handles, in attachment order.
    bool operator==(const FbKey& o) const {
      return pass == o.pass && width == o.width && height == o.height && views == o.views;
    }
  };
  struct FbKeyHash {
    std::size_t operator()(const FbKey& k) const {
      std::size_t h = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(k.pass));
      h ^= std::hash<uint64_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint64_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
      for (uint64_t v : k.views) {
        h ^= std::hash<uint64_t>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
      }
      return h;
    }
  };

  VkContext* ctx_ = nullptr;
  std::unordered_map<RenderPassKey, PassLayout, RenderPassKeyHash> passCache_;
  std::unordered_map<FbKey, VkFramebuffer, FbKeyHash> fbCache_;
  std::size_t passCreates_ = 0;
  std::size_t passHits_ = 0;
};

}  // namespace fable2::renderer::vulkan
