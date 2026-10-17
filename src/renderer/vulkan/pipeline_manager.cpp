// pipeline_manager.cpp — W4: descriptor sets & pipeline state (pipeline side).
#include "pipeline_manager.h"

#include "descriptor_manager.h"
#include "vk_context.h"

namespace fable2::renderer::vulkan {

uint64_t PipelineManager::HashSpirv(const std::vector<uint32_t>& spirv) {
  uint64_t h = 1469598103934665603ULL;
  const uint64_t f = 1099511628211ULL;
  for (uint32_t word : spirv) {
    for (int i = 0; i < 4; ++i) {
      h ^= (word >> (i * 8)) & 0xFF;
      h *= f;
    }
  }
  return h;
}

PipelineManager::PipelineManager(VkContext* ctx, DescriptorManager* descriptors)
    : ctx_(ctx), desc_(descriptors) {}

PipelineManager::~PipelineManager() { DestroyAll(); }

VkShaderModule PipelineManager::GetShaderModule(const std::vector<uint32_t>& spirv,
                                                VkShaderStageFlagBits stage) {
  (void)stage;
  const uint64_t hash = HashSpirv(spirv);
  auto it = shaderCache_.find(hash);
  if (it != shaderCache_.end()) {
    return it->second;
  }
  // W9: Validate SPIR-V with SPIRV-Tools before creating the VkShaderModule.
  // Non-fatal: if validation is unavailable or fails, log and continue.
  if (ctx_->spirvTools().IsValid()) {
    std::string valErr;
    if (!ctx_->spirvTools().Validate(spirv.data(), spirv.size(), valErr)) {
      std::printf("[warn] SPIR-V validation failed (hash %llu): %s\n",
                  (unsigned long long)hash, valErr.c_str());
    }
  }
  Functions& fn = ctx_->functions();
  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = spirv.size() * sizeof(uint32_t);
  sci.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (fn.vkCreateShaderModule(ctx_->device(), &sci, nullptr, &module) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  ++shaderCreates_;
  shaderCache_.emplace(hash, module);
  return module;
}

VkPipeline PipelineManager::GetPipeline(const PipelineKey& key, VkRenderPass renderPass,
                                        const std::vector<uint32_t>& vertexSpirv,
                                        const std::vector<uint32_t>& fragmentSpirv) {
  const uint64_t vsHash = HashSpirv(vertexSpirv);
  const uint64_t fsHash = HashSpirv(fragmentSpirv);
  CacheKey ckey{key, renderPass, vsHash, fsHash};
  auto it = pipelineCache_.find(ckey);
  if (it != pipelineCache_.end()) {
    ++pipelineHits_;
    return it->second;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  const VkShaderModule vsModule = GetShaderModule(vertexSpirv, VK_SHADER_STAGE_VERTEX_BIT);
  const VkShaderModule fsModule = GetShaderModule(fragmentSpirv, VK_SHADER_STAGE_FRAGMENT_BIT);
  if (vsModule == VK_NULL_HANDLE || fsModule == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  // Field order: {sType, pNext, flags, stage, module, pName, pSpecializationInfo}.
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_FRAGMENT_BIT, fsModule, "main", nullptr};

  // --- Vertex input state (baked). ---
  VkVertexInputBindingDescription binding = {};
  VkVertexInputAttributeDescription attributes[2] = {};
  uint32_t bindingCount = 0;
  uint32_t attributeCount = 0;
  switch (key.vertexInput) {
    case VertexInputLayout::kPos:
      binding = {0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
      attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      bindingCount = 1;
      attributeCount = 1;
      break;
    case VertexInputLayout::kPosTex:
      binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
      attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
      bindingCount = 1;
      attributeCount = 2;
      break;
    case VertexInputLayout::kNone:
    default:
      bindingCount = 0;
      attributeCount = 0;
      break;
  }
  VkPipelineVertexInputStateCreateInfo visci{};
  visci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  visci.vertexBindingDescriptionCount = bindingCount;
  visci.pVertexBindingDescriptions = bindingCount ? &binding : nullptr;
  visci.vertexAttributeDescriptionCount = attributeCount;
  visci.pVertexAttributeDescriptions = attributeCount ? attributes : nullptr;

  // --- Input assembly. ---
  VkPipelineInputAssemblyStateCreateInfo iasci{};
  iasci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  iasci.topology = key.topology;
  iasci.primitiveRestartEnable = key.primitiveRestart ? VK_TRUE : VK_FALSE;

  // --- Viewport state (dynamic). ---
  VkPipelineViewportStateCreateInfo vpsci{};
  vpsci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vpsci.viewportCount = key.viewportCount;
  vpsci.scissorCount = key.viewportCount;

  // --- Rasterization (line width + depth bias are dynamic). ---
  VkPipelineRasterizationStateCreateInfo rastci{};
  rastci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rastci.depthClampEnable = VK_FALSE;
  rastci.rasterizerDiscardEnable = VK_FALSE;
  rastci.polygonMode = key.fillMode;
  rastci.cullMode = key.cullMode;
  rastci.frontFace = key.frontFace;
  rastci.depthBiasEnable = VK_FALSE;
  rastci.lineWidth = 1.0f;

  // --- Multisample. ---
  VkPipelineMultisampleStateCreateInfo msaci{};
  msaci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  msaci.rasterizationSamples = key.samples;
  msaci.sampleShadingEnable = VK_FALSE;
  msaci.alphaToCoverageEnable = VK_FALSE;
  msaci.alphaToOneEnable = VK_FALSE;

  // --- Depth/stencil. ---
  VkPipelineDepthStencilStateCreateInfo dssci{};
  dssci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  dssci.depthTestEnable = key.depthTest ? VK_TRUE : VK_FALSE;
  dssci.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
  dssci.depthCompareOp = key.depthCompare;
  dssci.depthBoundsTestEnable = VK_TRUE;  // dynamic
  dssci.stencilTestEnable = VK_FALSE;

  // --- Color blend. ---
  VkPipelineColorBlendAttachmentState blend[1] = {};
  blend[0].blendEnable = key.alphaBlend ? VK_TRUE : VK_FALSE;
  if (key.alphaBlend) {
    blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend[0].colorBlendOp = VK_BLEND_OP_ADD;
    blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend[0].alphaBlendOp = VK_BLEND_OP_ADD;
  } else {
    blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend[0].colorBlendOp = VK_BLEND_OP_ADD;
    blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend[0].alphaBlendOp = VK_BLEND_OP_ADD;
  }
  blend[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cbsci{};
  cbsci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cbsci.attachmentCount = 1;
  cbsci.pAttachments = blend;

  // --- Dynamic state (the full set, to minimize pipeline variants). ---
  VkDynamicState dynamicStates[7] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_LINE_WIDTH,
      VK_DYNAMIC_STATE_DEPTH_BOUNDS,
      VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
  };
  VkPipelineDynamicStateCreateInfo dsci{};
  dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dsci.dynamicStateCount = 7;
  dsci.pDynamicStates = dynamicStates;

  VkGraphicsPipelineCreateInfo gpki{};
  gpki.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpki.stageCount = 2;
  gpki.pStages = stages;
  gpki.pVertexInputState = &visci;
  gpki.pInputAssemblyState = &iasci;
  gpki.pViewportState = &vpsci;
  gpki.pRasterizationState = &rastci;
  gpki.pMultisampleState = &msaci;
  gpki.pDepthStencilState = &dssci;
  gpki.pColorBlendState = &cbsci;
  gpki.pDynamicState = &dsci;
  gpki.layout = desc_ ? desc_->PipelineLayout() : VK_NULL_HANDLE;
  gpki.renderPass = renderPass;
  gpki.subpass = 0;

  // Pass the VkContext's pipeline cache (in-mem + on-disk) for the persistent
  // pipeline-cache win (vkCreateGraphicsPipelines takes it as the last arg).
  const VkPipelineCache pipelineCache = ctx_->pipelineCache();
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (fn.vkCreateGraphicsPipelines(device, pipelineCache, 1, &gpki, nullptr, &pipeline) !=
          VK_SUCCESS ||
      pipeline == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  ++pipelineCreates_;
  pipelineCache_.emplace(ckey, pipeline);
  return pipeline;
}

void PipelineManager::BindAndDraw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set,
                                  VkBuffer vertexBuffer, VkDeviceSize vertexOffset,
                                  const float blendConsts[4], uint32_t width, uint32_t height,
                                  uint32_t vertexCount, uint32_t instanceCount) {
  Functions& fn = ctx_->functions();
  fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  ++bindPipelineCalls_;

  if (set != VK_NULL_HANDLE) {
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desc_->PipelineLayout(), 0, 1,
                               &set, 0, nullptr);
    ++bindDescriptorSetCalls_;
  }

  const VkDeviceSize offsets[1] = {vertexOffset};
  const VkBuffer buffers[1] = {vertexBuffer};
  fn.vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);

  VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
  fn.vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {width, height}};
  fn.vkCmdSetScissor(cmd, 0, 1, &scissor);
  if (blendConsts) {
    fn.vkCmdSetBlendConstants(cmd, blendConsts);
  }
  fn.vkCmdSetLineWidth(cmd, 1.0f);
  const float depthBias[3] = {0.0f, 0.0f, 0.0f};
  fn.vkCmdSetDepthBias(cmd, depthBias[0], depthBias[1], depthBias[2]);
  const float depthBounds[2] = {0.0f, 1.0f};
  fn.vkCmdSetDepthBounds(cmd, depthBounds[0], depthBounds[1]);
  fn.vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);

  fn.vkCmdDraw(cmd, vertexCount, instanceCount, 0, 0);
  ++drawCalls_;
}

void PipelineManager::DestroyAll() {
  if (ctx_ == nullptr) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  for (auto& [hash, pipeline] : pipelineCache_) {
    if (pipeline) {
      fn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  pipelineCache_.clear();
  for (auto& [hash, pipeline] : multiPipelineCache_) {
    if (pipeline) {
      fn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  multiPipelineCache_.clear();
  for (auto& [hash, module] : shaderCache_) {
    if (module) {
      fn.vkDestroyShaderModule(device, module, nullptr);
    }
  }
  shaderCache_.clear();
}

VkPipeline PipelineManager::GetPipelineMulti(const PipelineKey& key, VkRenderPass renderPass,
                                             const std::vector<uint32_t>& vsSpirv,
                                             const std::vector<uint32_t>& hsSpirv,
                                             const std::vector<uint32_t>& dsSpirv,
                                             const std::vector<uint32_t>& gsSpirv,
                                             const std::vector<uint32_t>& fsSpirv,
                                             uint32_t patchControlPoints) {
  if (vsSpirv.empty() || fsSpirv.empty()) {
    return VK_NULL_HANDLE;
  }
  const uint64_t vsHash = HashSpirv(vsSpirv);
  const uint64_t hsHash = hsSpirv.empty() ? 0 : HashSpirv(hsSpirv);
  const uint64_t dsHash = dsSpirv.empty() ? 0 : HashSpirv(dsSpirv);
  const uint64_t gsHash = gsSpirv.empty() ? 0 : HashSpirv(gsSpirv);
  const uint64_t fsHash = HashSpirv(fsSpirv);

  MultiCacheKey mkey{key, renderPass, vsHash, hsHash, dsHash, gsHash, fsHash, patchControlPoints};
  auto it = multiPipelineCache_.find(mkey);
  if (it != multiPipelineCache_.end()) {
    ++multiPipelineHits_;
    return it->second;
  }

  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  // Create shader modules for each present stage.
  const VkShaderModule vsModule = GetShaderModule(vsSpirv, VK_SHADER_STAGE_VERTEX_BIT);
  const VkShaderModule hsModule =
      hsSpirv.empty() ? VK_NULL_HANDLE : GetShaderModule(hsSpirv, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
  const VkShaderModule dsModule =
      dsSpirv.empty() ? VK_NULL_HANDLE : GetShaderModule(dsSpirv, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
  const VkShaderModule gsModule =
      gsSpirv.empty() ? VK_NULL_HANDLE : GetShaderModule(gsSpirv, VK_SHADER_STAGE_GEOMETRY_BIT);
  const VkShaderModule fsModule = GetShaderModule(fsSpirv, VK_SHADER_STAGE_FRAGMENT_BIT);
  if (vsModule == VK_NULL_HANDLE || fsModule == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  if ((hsSpirv.empty() != dsSpirv.empty()) && !hsSpirv.empty()) {
    // HS and DS must be present together for tessellation.
    return VK_NULL_HANDLE;
  }

  const bool tessellated = (hsModule != VK_NULL_HANDLE && dsModule != VK_NULL_HANDLE);

  // Build the stage array (up to 5 stages).
  VkPipelineShaderStageCreateInfo stages[5] = {};
  uint32_t stageCount = 0;

  stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                        VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr};
  stages[stageCount++].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  if (tessellated) {
    stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, hsModule, "main", nullptr};
    stages[stageCount++].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, dsModule, "main", nullptr};
    stages[stageCount++].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  }
  if (gsModule != VK_NULL_HANDLE) {
    stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_GEOMETRY_BIT, gsModule, "main", nullptr};
    stages[stageCount++].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  }
  stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                        VK_SHADER_STAGE_FRAGMENT_BIT, fsModule, "main", nullptr};
  stages[stageCount++].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

  // Vertex input state (same as GetPipeline).
  VkVertexInputBindingDescription binding = {};
  VkVertexInputAttributeDescription attributes[2] = {};
  uint32_t bindingCount = 0;
  uint32_t attributeCount = 0;
  switch (key.vertexInput) {
    case VertexInputLayout::kPos:
      binding = {0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
      attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      bindingCount = 1; attributeCount = 1;
      break;
    case VertexInputLayout::kPosTex:
      binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
      attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
      bindingCount = 1; attributeCount = 2;
      break;
    default:
      break;
  }
  VkPipelineVertexInputStateCreateInfo visci{};
  visci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  visci.vertexBindingDescriptionCount = bindingCount;
  visci.pVertexBindingDescriptions = bindingCount ? &binding : nullptr;
  visci.vertexAttributeDescriptionCount = attributeCount;
  visci.pVertexAttributeDescriptions = attributeCount ? attributes : nullptr;

  // Input assembly.
  VkPipelineInputAssemblyStateCreateInfo iasci{};
  iasci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  iasci.topology = key.topology;
  iasci.primitiveRestartEnable = key.primitiveRestart ? VK_TRUE : VK_FALSE;

  // Viewport state (dynamic).
  VkPipelineViewportStateCreateInfo vpsci{};
  vpsci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vpsci.viewportCount = key.viewportCount;
  vpsci.scissorCount = key.viewportCount;

  // Rasterization.
  VkPipelineRasterizationStateCreateInfo rastci{};
  rastci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rastci.depthClampEnable = VK_FALSE;
  rastci.rasterizerDiscardEnable = VK_FALSE;
  rastci.polygonMode = key.fillMode;
  rastci.cullMode = key.cullMode;
  rastci.frontFace = key.frontFace;
  rastci.depthBiasEnable = VK_FALSE;
  rastci.lineWidth = 1.0f;

  // Tessellation state (only for tessellated pipelines).
  VkPipelineTessellationStateCreateInfo tessci{};
  if (tessellated) {
    tessci.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessci.patchControlPoints = patchControlPoints;
  }

  // Multisample.
  VkPipelineMultisampleStateCreateInfo msaci{};
  msaci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  msaci.rasterizationSamples = key.samples;
  msaci.sampleShadingEnable = VK_FALSE;
  msaci.alphaToCoverageEnable = VK_FALSE;
  msaci.alphaToOneEnable = VK_FALSE;

  // Depth/stencil.
  VkPipelineDepthStencilStateCreateInfo dssci{};
  dssci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  dssci.depthTestEnable = key.depthTest ? VK_TRUE : VK_FALSE;
  dssci.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
  dssci.depthCompareOp = key.depthCompare;
  dssci.depthBoundsTestEnable = VK_TRUE;
  dssci.stencilTestEnable = VK_FALSE;

  // Color blend.
  VkPipelineColorBlendAttachmentState blend[1] = {};
  blend[0].blendEnable = key.alphaBlend ? VK_TRUE : VK_FALSE;
  if (key.alphaBlend) {
    blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend[0].colorBlendOp = VK_BLEND_OP_ADD;
    blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend[0].alphaBlendOp = VK_BLEND_OP_ADD;
  } else {
    blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend[0].colorBlendOp = VK_BLEND_OP_ADD;
    blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend[0].alphaBlendOp = VK_BLEND_OP_ADD;
  }
  blend[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cbsci{};
  cbsci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cbsci.attachmentCount = 1;
  cbsci.pAttachments = blend;

  // Dynamic state.
  VkDynamicState dynamicStates[7] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
      VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE};
  VkPipelineDynamicStateCreateInfo dsci{};
  dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dsci.dynamicStateCount = 7;
  dsci.pDynamicStates = dynamicStates;

  VkGraphicsPipelineCreateInfo gpki{};
  gpki.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpki.stageCount = stageCount;
  gpki.pStages = stages;
  gpki.pVertexInputState = &visci;
  gpki.pInputAssemblyState = &iasci;
  gpki.pTessellationState = tessellated ? &tessci : nullptr;
  gpki.pViewportState = &vpsci;
  gpki.pRasterizationState = &rastci;
  gpki.pMultisampleState = &msaci;
  gpki.pDepthStencilState = &dssci;
  gpki.pColorBlendState = &cbsci;
  gpki.pDynamicState = &dsci;
  gpki.layout = desc_ ? desc_->PipelineLayout() : VK_NULL_HANDLE;
  gpki.renderPass = renderPass;
  gpki.subpass = 0;

  const VkPipelineCache pipelineCache = ctx_->pipelineCache();
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (fn.vkCreateGraphicsPipelines(device, pipelineCache, 1, &gpki, nullptr, &pipeline) !=
          VK_SUCCESS ||
      pipeline == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  ++multiPipelineCreates_;
  multiPipelineCache_.emplace(mkey, pipeline);
  return pipeline;
}

}  // namespace fable2::renderer::vulkan
