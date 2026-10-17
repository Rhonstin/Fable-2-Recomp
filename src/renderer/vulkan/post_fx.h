// W10 — Post-FX passes: FXAA + gamma correction.
//
// Lightweight fullscreen fragment shader passes applied after the guest
// scene render, before compositing to the swapchain. Reuses the
// DescriptorManager for texture bindings and the PipelineManager for
// pipeline assembly.
//
//   - Gamma: vec4(pow(color, vec3(1.0/gamma)))
//   - FXAA:  simplified FXAA (luma-based edge detection + blend)

#pragma once

#include "vk_context.h"
#include "descriptor_manager.h"
#include "pipeline_manager.h"
#include "render_pass_manager.h"

namespace fable2::renderer::vulkan {

// Post-FX pipeline state.
class PostFX {
 public:
  PostFX() = default;
  ~PostFX();

  // Initialize the post-FX pipelines. Must be called after the
  // DescriptorManager and PipelineManager are ready.
  bool Init(VkContext* ctx, DescriptorManager* descriptors, PipelineManager* pipelines,
            RenderPassManager* renderPasses, std::string& err);
  void Shutdown();

  // Apply gamma correction to a fullscreen image.
  // `inputImage` is the source (guest scene render), `outputImage` is the
  // destination. Both must be the same size and format (R8G8B8A8).
  void ApplyGamma(VkCommandBuffer cmd, VkImage inputImage, VkImage outputImage,
                  uint32_t width, uint32_t height);

  // Apply FXAA to a fullscreen image.
  void ApplyFXAA(VkCommandBuffer cmd, VkImage inputImage, VkImage outputImage,
                 uint32_t width, uint32_t height);

  // Accessors.
  uint64_t gammaPasses = 0;
  uint64_t fxaaPasses = 0;

 private:
  VkContext* ctx_ = nullptr;
  DescriptorManager* descriptors_ = nullptr;
  PipelineManager* pipelines_ = nullptr;
  RenderPassManager* renderPasses_ = nullptr;

  VkPipeline gammaPipeline_ = VK_NULL_HANDLE;
  VkPipeline fxaaPipeline_ = VK_NULL_HANDLE;
  VkRenderPass gammaPass_ = VK_NULL_HANDLE;
  VkRenderPass fxaaPass_ = VK_NULL_HANDLE;

  // Vertex buffer for the fullscreen quad.
  VkBuffer quadBuffer_ = VK_NULL_HANDLE;
  VmaAllocation quadAlloc_ = VK_NULL_HANDLE;

  bool initialized_ = false;
};

}  // namespace fable2::renderer::vulkan
