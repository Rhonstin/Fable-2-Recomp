// descriptor_manager.h — W4: descriptor sets & pipeline state (descriptor side).
//
// A small, shared, cache-backed descriptor system built on VkContext:
//   - a tiny number of STABLE descriptor set layouts (a shared "shader" set with
//     the cbuffer UBO, a bindless sampled-image array, a canonical sampler
//     array, and extra UBO banks) so pipelines share layouts instead of each
//     declaring their own;
//   - a SAMPLER cache that dedups identical sampler states (a handful of
//     canonical samplers + dynamic for the rest);
//   - a preallocated, per-frame-reset DESCRIPTOR POOL (zero allocations per
//     draw);
//   - a single mapped host-visible CONSTANT BUFFER upload ring for all immediate
//     cbuffer data (minimizes per-draw vkUpdateBuffer);
//   - BINDLESS texture/RT binding: index-based image arrays so one descriptor
//     set can reference any cached image (key to avoiding per-texture churn).
//
// Namespace: fable2::renderer::vulkan
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <vk_mem_alloc.h>

#include <vulkan/vulkan.h>

namespace fable2::renderer::vulkan {

class VkContext;

// A canonical sampler description. Identical configs share one VkSampler.
struct SamplerKey {
  VkFilter magFilter = VK_FILTER_LINEAR;
  VkFilter minFilter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkSamplerAddressMode uMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  VkSamplerAddressMode vMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  VkSamplerAddressMode wMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  float mipLodBias = 0.0f;
  bool anisotropy = false;
  float maxAnisotropy = 1.0f;
  bool compare = false;
  VkCompareOp compareOp = VK_COMPARE_OP_NEVER;
  bool minLodClamp = false;
  bool maxLodClamp = false;
  float minLod = 0.0f;
  float maxLod = 100.0f;
  VkBorderColor borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

  bool operator==(const SamplerKey& o) const {
    return magFilter == o.magFilter && minFilter == o.minFilter &&
           mipmapMode == o.mipmapMode && uMode == o.uMode && vMode == o.vMode &&
           wMode == o.wMode && mipLodBias == o.mipLodBias && anisotropy == o.anisotropy &&
           maxAnisotropy == o.maxAnisotropy && compare == o.compare &&
           compareOp == o.compareOp && minLodClamp == o.minLodClamp &&
           maxLodClamp == o.maxLodClamp && minLod == o.minLod && maxLod == o.maxLod &&
           borderColor == o.borderColor;
  }
};
struct SamplerKeyHash {
  std::size_t operator()(const SamplerKey& k) const {
    auto mix = [&k](std::size_t h, uint64_t v) {
      const uint64_t f = 1099511628211ULL;
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= f;
      }
      return h;
    };
    std::size_t h = 1469598103934665603ULL;
    h = mix(h, uint64_t(k.magFilter));
    h = mix(h, uint64_t(k.minFilter));
    h = mix(h, uint64_t(k.mipmapMode));
    h = mix(h, uint64_t(k.uMode));
    h = mix(h, uint64_t(k.vMode));
    h = mix(h, uint64_t(k.wMode));
    h = mix(h, uint64_t(k.mipLodBias));
    h = mix(h, uint64_t(k.anisotropy ? 1 : 0));
    h = mix(h, uint64_t(k.compare ? 1 : 0));
    h = mix(h, uint64_t(k.compareOp));
    h = mix(h, uint64_t(k.minLodClamp ? 1 : 0));
    h = mix(h, uint64_t(k.maxLodClamp ? 1 : 0));
    h = mix(h, uint64_t(k.maxLod));
    return h;
  }
};

// The binding slots of the shared "shader" descriptor set (set 0).
namespace ShaderBindings {
constexpr uint32_t kCbuffer = 0;  // binding 0: main cbuffer (uniform buffer)
constexpr uint32_t kTextures = 1;  // binding 1: bindless sampled-image array
constexpr uint32_t kSamplers = 2;  // binding 2: canonical sampler array
constexpr uint32_t kUniforms = 3;  // binding 3: extra UBO banks (UBO/SSBO/ROBO)
constexpr uint32_t kCbufferBankCount = 8;  // extra uniform buffers at binding 3
}  // namespace ShaderBindings

struct DescriptorManagerConfig {
  uint32_t maxTextureBindings = 1024;   // bindless sampled-image array size
  uint32_t maxSamplerBindings = 64;     // canonical sampler array size
  uint32_t poolSetCapacity = 1024;      // descriptor sets per pool (per-frame reset)
  uint64_t uploadRingBytes = 256 * 1024;  // constant buffer upload ring size
};

class DescriptorManager {
 public:
  explicit DescriptorManager(VkContext* ctx);
  ~DescriptorManager();
  DescriptorManager(const DescriptorManager&) = delete;
  DescriptorManager& operator=(const DescriptorManager&) = delete;

  // Create the shared set layout, pool, samplers, and upload ring.
  bool Create(const DescriptorManagerConfig& config, std::string& err);

  // The shared shader descriptor set layout (set 0).
  VkDescriptorSetLayout ShaderSetLayout() const { return shaderSetLayout_; }
  // The shared pipeline layout (one descriptor set = shader set).
  VkPipelineLayout PipelineLayout() const { return pipelineLayout_; }

  // Get (or create + cache) a sampler for this config.
  VkSampler GetSampler(const SamplerKey& key);

  // Per-frame: reset the descriptor pool + the constant buffer upload ring.
  void BeginFrame();

  // Allocate a descriptor set for the shared shader layout (from the pool).
  VkDescriptorSet AllocShaderSet();

  // Write descriptor entries.
  void WriteUniform(VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize offset,
                    VkDeviceSize size);
  void WriteUniformBank(VkDescriptorSet set, uint32_t bankIndex, VkBuffer buffer, VkDeviceSize offset,
                        VkDeviceSize size);
  void WriteTexture(VkDescriptorSet set, uint32_t textureIndex, VkImageView view,
                    VkImageLayout layout);
  void WriteSampler(VkDescriptorSet set, uint32_t samplerIndex, VkSampler sampler);

  // Constant buffer upload ring: copy `size` bytes of `data` into the ring and
  // return (buffer, offset) to bind as the cbuffer UBO.
  bool StageConstants(const void* data, uint32_t size, VkBuffer* outBuffer,
                      VkDeviceSize* outOffset);
  VkBuffer UploadBuffer() const { return uploadBuffer_; }

  // Diagnostics.
  std::size_t samplerCount() const { return samplerCache_.size(); }
  std::size_t samplerCreates() const { return samplerCreates_; }
  std::size_t samplerHits() const { return samplerHits_; }
  std::size_t poolAllocs() const { return poolAllocs_; }
  uint64_t uploadRingUsed() const { return uploadCursor_; }

  // Destroy all samplers, the pool, the set layout, the pipeline layout, and
  // the upload buffer.
  void DestroyAll();

 private:
  VkContext* ctx_ = nullptr;
  VkDescriptorSetLayout shaderSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkBuffer uploadBuffer_ = VK_NULL_HANDLE;
  VmaAllocation uploadAlloc_ = VK_NULL_HANDLE;
  void* uploadMap_ = nullptr;
  uint64_t uploadCursor_ = 0;

  std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplerCache_;
  std::size_t samplerCreates_ = 0;
  std::size_t samplerHits_ = 0;
  std::size_t poolAllocs_ = 0;

  DescriptorManagerConfig config_{};
};

}  // namespace fable2::renderer::vulkan
