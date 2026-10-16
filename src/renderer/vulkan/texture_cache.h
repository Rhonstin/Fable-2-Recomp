#pragma once

// W5: the guest texture cache (image manager) on VkContext.
//
// Mirrors the SDK's VulkanTextureCache conceptually (keyed guest textures,
// format mapping, refcount + eviction, GPU mip generation) but is self-contained
// and focused on the Fable 2 hot paths:
//   * keyed by (guest address, size, format, mip level, protection),
//   * reference-counted with LRU-ish eviction to a memory budget,
//   * a clean Xenos->Vulkan format table that flags which formats are
//     "native" (guest bytes map 1:1 to a VkFormat, so no per-pixel CPU->GPU
//     conversion is needed — the big Fable 2 streaming-texture lever),
//   * GPU mip generation via vkCmdBlitImage,
//   * UNDEFINED -> SHADER_READ_ONLY layout transitions + aliasing of freed
//     memory,
//   * sampler-view composition (a VkDescriptorImageInfo pairing a cached image
//     view with a canonical sampler from the DescriptorManager),
//   * a "split memory" decision: whether the guest source is host-visible
//     (direct upload/alias) or device-local (needs a staging copy),
//   * the ability to adopt an external VkImage (decoded video frames).

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// A Xenos texture format mapped to its host VkFormat.
struct XenosFormatInfo {
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;
  // Whether the guest byte layout maps 1:1 to vkFormat (with at most a
  // component swizzle, which the load shader / sampler handles for free). When
  // true the guest memory can be uploaded directly — no per-pixel CPU->GPU
  // format conversion. When false a conversion pass (load shader / CPU) is
  // required because the bit widths don't line up (e.g. 11-bit -> 16-bit).
  bool native = true;
  // Block size for block-compressed formats (4x4 for BCn), else 1x1.
  uint32_t blockW = 1;
  uint32_t blockH = 1;
};

// Maps a Xenos TextureFormat (xenos::TextureFormat ordinal) to a host format.
// Returns false for formats with no host representation.
bool XenosToVulkanFormat(uint32_t xenosFormat, XenosFormatInfo& out);

// The guest key identifying a texture. Matches the W5 plan: (guest address,
// size, format, mip level, protection).
struct GuestTextureKey {
  uint64_t guestAddress = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipLevels = 1;
  uint32_t format = 0;      // xenos::TextureFormat ordinal
  uint32_t protection = 0;  // 0 = read-only, 1 = read/write (streaming)

  bool operator==(const GuestTextureKey& o) const {
    return guestAddress == o.guestAddress && width == o.width && height == o.height &&
           mipLevels == o.mipLevels && format == o.format && protection == o.protection;
  }
};

struct GuestTextureKeyHash {
  size_t operator()(const GuestTextureKey& k) const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) {
      h ^= v;
      h *= 1099511628211ULL;
    };
    mix(k.guestAddress);
    mix(k.width);
    mix(k.height);
    mix(k.mipLevels);
    mix(k.format);
    mix(k.protection);
    return static_cast<size_t>(h);
  }
};

// A cached guest texture: a VkImage + VkImageView for one guest texture.
struct GuestTexture {
  GuestTextureKey key;
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool native = true;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipLevels = 1;
  uint64_t bytes = 0;         // host image memory in bytes
  uint32_t refCount = 0;      // guest binding reference count
  uint64_t lastUsedFrame = 0; // LRU eviction ordering
  bool mipsGenerated = false;
  bool external = false;      // adopted external image (video frame); not freed
  VkImageLayout lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

class TextureCache {
 public:
  TextureCache() = default;
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void Init(VkContext* ctx) {
    ctx_ = ctx;
  }

  // Get or create a texture for the given key (without a ref). Returns the
  // existing texture if present, else allocates the image + view.
  GuestTexture* CreateOrUpdate(const GuestTextureKey& key, uint32_t frame);
  // Find an existing texture (no create, no ref).
  GuestTexture* Find(const GuestTextureKey& key) const;
  // Add a guest binding reference. Returns the texture (created if missing).
  GuestTexture* Acquire(const GuestTextureKey& key, uint32_t frame);
  // Drop a reference; evicts (frees image + view) when it hits 0 and is
  // eligible. Returns the number of bytes freed (0 if not evicted).
  uint64_t Release(GuestTexture* tex, uint32_t frame);
  // Evict LRU textures down to the byte budget (only refcount-0 textures).
  uint32_t EvictToBudget(uint64_t budgetBytes);

  // Upload a texture's level-0 data from a CPU pointer (simulating guest memory).
  //   sourceHostVisible: true = the guest source is host-visible (a direct copy
  //     from the mapped guest memory; the "alias/native" fast path);
  //     false = the guest source is device-local and must be staged through a
  //     host-visible staging buffer (the "split memory" slow path).
  // Copies the base level into the image and transitions it to
  // TRANSFER_DST_OPTIMAL. Returns true on success.
  bool UploadLevel0(GuestTexture* tex, const void* cpuData, uint32_t dataSize,
                    VkCommandBuffer cmd, bool sourceHostVisible);
  // GPU-generate the remaining mip levels from level 0 via vkCmdBlitImage, then
  // transition the whole range to SHADER_READ_ONLY_OPTIMAL.
  void GenerateMips(GuestTexture* tex, VkCommandBuffer cmd);
  // Transition a level range to the given layout (UNDEFINED -> X).
  void SetLayout(GuestTexture* tex, VkImageLayout layout, VkCommandBuffer cmd);

  // Sampler-view composition: a VkDescriptorImageInfo pairing this texture's
  // image view with a canonical sampler, ready to write into a descriptor set.
  static VkDescriptorImageInfo MakeSampleInfo(const GuestTexture* tex, VkSampler sampler,
                                              VkImageLayout layout);

  // Adopt an external VkImage + view as a cached texture (decoded video frame).
  // external = true marks it as not owned by the cache (not freed on evict).
  GuestTexture* AdoptExternal(const GuestTextureKey& key, VkImage image, VkImageView view,
                              VkFormat format, uint32_t frame);

  uint64_t TotalBytes() const { return totalBytes_; }
  size_t TextureCount() const { return textures_.size(); }
  const std::unordered_map<GuestTextureKey, GuestTexture, GuestTextureKeyHash>& Textures() const {
    return textures_;
  }
  uint32_t evicted = 0;  // total textures evicted (freed)
  uint64_t uploadsHostVisible = 0;  // UploadLevel0 fast-path count
  uint64_t uploadsStaged = 0;       // UploadLevel0 staging count

  void DestroyAll();

 private:
  GuestTexture* CreateTexture(const GuestTextureKey& key, uint32_t frame);
  void FreeTexture(GuestTexture* tex);
  // Grow the persistent host-visible upload buffer to at least `size` bytes.
  bool EnsureUploadBuffer(uint32_t size, std::string& err);

  VkContext* ctx_ = nullptr;
  std::unordered_map<GuestTextureKey, GuestTexture, GuestTextureKeyHash> textures_;
  uint64_t totalBytes_ = 0;
  VkBuffer uploadBuffer_ = VK_NULL_HANDLE;
  VmaAllocation uploadAlloc_ = VK_NULL_HANDLE;
  void* uploadMap_ = nullptr;
  uint32_t uploadCapacity_ = 0;
};

}  // namespace fable2::renderer::vulkan
