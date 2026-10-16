// pipeline_manager.h — W4: descriptor sets & pipeline state (pipeline side).
//
// A cache-backed graphics-pipeline builder built on VkContext + the
// DescriptorManager's shared pipeline layout. The goal is STEADY-STATE
// PIPELINE REUSE: consecutive draws that share pipeline state hit the cache and
// rebind the same VkPipeline instead of re-creating it (Xenia's per-draw
// re-creation is the main cost this removes).
//
// Pipelines are keyed on (vertex input layout, rasterizer, depth/stencil,
// blend, sample count, shader stage hashes, dynamic-state subset, viewport
// count, render pass). The full dynamic-state set (viewport, scissor, blend
// constants, depth bias, line width, depth bounds, rasterizer discard) is
// enabled to keep the number of variants small.
//
// Namespace: fable2::renderer::vulkan
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace fable2::renderer::vulkan {

class VkContext;
class DescriptorManager;

// The vertex input layout (baked into the pipeline; the rest of the state is
// dynamic).
enum class VertexInputLayout : uint32_t {
  kNone = 0,  // no attributes (uses gl_VertexIndex)
  kPos = 1,  // vec3 position @ location 0
  kPosTex = 2,  // vec3 position @ 0 + vec2 texcoord @ 1
};

// The pipeline-state key. Combined with the render pass and the shader stage
// hashes, this fully determines a VkPipeline.
struct PipelineKey {
  VertexInputLayout vertexInput = VertexInputLayout::kPosTex;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool primitiveRestart = false;
  VkPolygonMode fillMode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
  VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  bool depthTest = true;
  bool depthWrite = false;
  VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  bool alphaBlend = true;
  uint32_t viewportCount = 1;

  bool operator==(const PipelineKey& o) const {
    return vertexInput == o.vertexInput && topology == o.topology &&
           primitiveRestart == o.primitiveRestart && fillMode == o.fillMode &&
           cullMode == o.cullMode && frontFace == o.frontFace && depthTest == o.depthTest &&
           depthWrite == o.depthWrite && depthCompare == o.depthCompare &&
           samples == o.samples && alphaBlend == o.alphaBlend &&
           viewportCount == o.viewportCount;
  }
};
struct PipelineKeyHash {
  std::size_t operator()(const PipelineKey& k) const {
    auto mix = [&k](std::size_t h, uint64_t v) {
      const uint64_t f = 1099511628211ULL;
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= f;
      }
      return h;
    };
    std::size_t h = 1469598103934665603ULL;
    h = mix(h, uint64_t(k.vertexInput));
    h = mix(h, uint64_t(k.topology));
    h = mix(h, uint64_t(k.primitiveRestart ? 1 : 0));
    h = mix(h, uint64_t(k.fillMode));
    h = mix(h, uint64_t(k.cullMode));
    h = mix(h, uint64_t(k.frontFace));
    h = mix(h, uint64_t(k.depthTest ? 1 : 0));
    h = mix(h, uint64_t(k.depthWrite ? 1 : 0));
    h = mix(h, uint64_t(k.depthCompare));
    h = mix(h, uint64_t(k.samples));
    h = mix(h, uint64_t(k.alphaBlend ? 1 : 0));
    h = mix(h, uint64_t(k.viewportCount));
    return h;
  }
};

class PipelineManager {
 public:
  PipelineManager(VkContext* ctx, DescriptorManager* descriptors);
  ~PipelineManager();
  PipelineManager(const PipelineManager&) = delete;
  PipelineManager& operator=(const PipelineManager&) = delete;

  // Get (or create + cache) a shader module from SPIR-V.
  VkShaderModule GetShaderModule(const std::vector<uint32_t>& spirv,
                                 VkShaderStageFlagBits stage);

  // Get (or create + cache) a graphics pipeline for `key` + `renderPass` with
  // the given vertex/fragment SPIR-V. The shader hashes are folded into the
  // cache key. Returns VK_NULL_HANDLE on creation failure.
  VkPipeline GetPipeline(const PipelineKey& key, VkRenderPass renderPass,
                         const std::vector<uint32_t>& vertexSpirv,
                         const std::vector<uint32_t>& fragmentSpirv);

  // A per-pipeline draw: bind the pipeline, bind the descriptor set, set the
  // dynamic state (viewport/scissor/blend/depth bias/line width), and draw.
  // `vertexBuffer` + `vertexOffset` are the single vertex buffer (binding 0).
  void BindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set,
                   VkBuffer vertexBuffer, VkDeviceSize vertexOffset, const float blendConsts[4],
                   uint32_t width, uint32_t height, uint32_t vertexCount,
                   uint32_t instanceCount = 1);

  // Diagnostics.
  std::size_t shaderCount() const { return shaderCache_.size(); }
  std::size_t shaderCreates() const { return shaderCreates_; }
  std::size_t pipelineCount() const { return pipelineCache_.size(); }
  std::size_t pipelineCreates() const { return pipelineCreates_; }
  std::size_t pipelineHits() const { return pipelineHits_; }
  std::size_t bindPipelineCalls() const { return bindPipelineCalls_; }
  std::size_t bindDescriptorSetCalls() const { return bindDescriptorSetCalls_; }
  std::size_t drawCalls() const { return drawCalls_; }

  // Destroy every cached shader + pipeline.
  void DestroyAll();

 private:
  struct CacheKey {
    PipelineKey state;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint64_t vsHash = 0;
    uint64_t fsHash = 0;
    bool operator==(const CacheKey& o) const {
      return state == o.state && renderPass == o.renderPass && vsHash == o.vsHash &&
             fsHash == o.fsHash;
    }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const {
      std::size_t h = PipelineKeyHash{}(k.state);
      h ^= std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(k.renderPass)) + 0x9e3779b9 +
           (h << 6) + (h >> 2);
      h ^= std::hash<uint64_t>{}(k.vsHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<uint64_t>{}(k.fsHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  // FNV-1a over SPIR-V bytes.
  static uint64_t HashSpirv(const std::vector<uint32_t>& spirv);

  VkContext* ctx_ = nullptr;
  DescriptorManager* desc_ = nullptr;
  std::unordered_map<uint64_t, VkShaderModule> shaderCache_;
  std::unordered_map<CacheKey, VkPipeline, CacheKeyHash> pipelineCache_;
  std::size_t shaderCreates_ = 0;
  std::size_t pipelineCreates_ = 0;
  std::size_t pipelineHits_ = 0;
  std::size_t bindPipelineCalls_ = 0;
  std::size_t bindDescriptorSetCalls_ = 0;
  std::size_t drawCalls_ = 0;
};

}  // namespace fable2::renderer::vulkan
