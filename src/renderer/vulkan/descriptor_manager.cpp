// descriptor_manager.cpp — W4: descriptor sets & pipeline state (descriptor side).
#include "descriptor_manager.h"

#include <cfloat>
#include <cstring>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

DescriptorManager::DescriptorManager(VkContext* ctx) : ctx_(ctx) {}

DescriptorManager::~DescriptorManager() { DestroyAll(); }

bool DescriptorManager::Create(const DescriptorManagerConfig& config, std::string& err) {
  config_ = config;
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  // --- Shared shader descriptor set layout (set 0). ---
  VkDescriptorSetLayoutBinding bindings[4] = {};
  // Field order: {binding, descriptorType, descriptorCount, stageFlags, pImmutableSamplers}.
  bindings[ShaderBindings::kCbuffer] = {
      ShaderBindings::kCbuffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[ShaderBindings::kTextures] = {
      ShaderBindings::kTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, config.maxTextureBindings,
      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[ShaderBindings::kSamplers] = {
      ShaderBindings::kSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, config.maxSamplerBindings,
      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  bindings[ShaderBindings::kUniforms] = {
      ShaderBindings::kUniforms, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ShaderBindings::kCbufferBankCount,
      VK_SHADER_STAGE_ALL, nullptr};
  VkDescriptorSetLayoutCreateInfo slci{};
  slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci.bindingCount = 4;
  slci.pBindings = bindings;
  if (fn.vkCreateDescriptorSetLayout(device, &slci, nullptr, &shaderSetLayout_) != VK_SUCCESS) {
    err = "vkCreateDescriptorSetLayout (shader set) failed";
    return false;
  }

  // --- Shared pipeline layout (one set = shader set). ---
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &shaderSetLayout_;
  if (fn.vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) {
    err = "vkCreatePipelineLayout failed";
    return false;
  }

  // --- Descriptor pool (per-frame reset). ---
  const uint32_t uniformPerSet = 1 + ShaderBindings::kCbufferBankCount;
  VkDescriptorPoolSize sizes[3] = {};
  sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sizes[0].descriptorCount = config.poolSetCapacity * uniformPerSet;
  sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[1].descriptorCount = config.poolSetCapacity * config.maxTextureBindings;
  sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  sizes[2].descriptorCount = config.poolSetCapacity * config.maxSamplerBindings;
  VkDescriptorPoolCreateInfo dpcci{};
  dpcci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpcci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpcci.maxSets = config.poolSetCapacity;
  dpcci.poolSizeCount = 3;
  dpcci.pPoolSizes = sizes;
  if (fn.vkCreateDescriptorPool(device, &dpcci, nullptr, &pool_) != VK_SUCCESS) {
    err = "vkCreateDescriptorPool failed";
    return false;
  }

  // --- Constant buffer upload ring (host-visible, mapped). ---
  if (!ctx_->AllocateBuffer(&uploadBuffer_, &uploadAlloc_, config_.uploadRingBytes,
                            VMA_MEMORY_USAGE_CPU_TO_GPU, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uploadMap_, err)) {
    return false;
  }

  BeginFrame();
  return true;
}

VkSampler DescriptorManager::GetSampler(const SamplerKey& key) {
  auto it = samplerCache_.find(key);
  if (it != samplerCache_.end()) {
    ++samplerHits_;
    return it->second;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = key.magFilter;
  sci.minFilter = key.minFilter;
  sci.mipmapMode = key.mipmapMode;
  sci.addressModeU = key.uMode;
  sci.addressModeV = key.vMode;
  sci.addressModeW = key.wMode;
  sci.mipLodBias = key.mipLodBias;
  sci.anisotropyEnable = key.anisotropy ? VK_TRUE : VK_FALSE;
  sci.maxAnisotropy = key.maxAnisotropy;
  sci.compareEnable = key.compare ? VK_TRUE : VK_FALSE;
  sci.compareOp = key.compareOp;
  sci.minLod = key.minLodClamp ? key.minLod : 0.0f;
  sci.maxLod = key.maxLodClamp ? key.maxLod : FLT_MAX;
  sci.borderColor = key.borderColor;
  sci.unnormalizedCoordinates = VK_FALSE;
  VkSampler sampler = VK_NULL_HANDLE;
  if (fn.vkCreateSampler(device, &sci, nullptr, &sampler) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  ++samplerCreates_;
  samplerCache_.emplace(key, sampler);
  return sampler;
}

void DescriptorManager::BeginFrame() {
  if (pool_) {
    Functions& fn = ctx_->functions();
    const VkDescriptorPoolResetFlags flags = 0;
    fn.vkResetDescriptorPool(ctx_->device(), pool_, flags);
  }
  uploadCursor_ = 0;
}

VkDescriptorSet DescriptorManager::AllocShaderSet() {
  if (!pool_) {
    return VK_NULL_HANDLE;
  }
  Functions& fn = ctx_->functions();
  VkDescriptorSetAllocateInfo ainfo{};
  ainfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ainfo.descriptorPool = pool_;
  ainfo.descriptorSetCount = 1;
  ainfo.pSetLayouts = &shaderSetLayout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (fn.vkAllocateDescriptorSets(ctx_->device(), &ainfo, &set) != VK_SUCCESS ||
      set == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  ++poolAllocs_;
  return set;
}

void DescriptorManager::WriteUniform(VkDescriptorSet set, uint32_t binding, VkBuffer buffer,
                                     VkDeviceSize offset, VkDeviceSize size) {
  VkDescriptorBufferInfo bi{buffer, offset, size};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set;
  w.dstBinding = binding;
  w.dstArrayElement = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &bi;
  ctx_->functions().vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

void DescriptorManager::WriteUniformBank(VkDescriptorSet set, uint32_t bankIndex, VkBuffer buffer,
                                         VkDeviceSize offset, VkDeviceSize size) {
  VkDescriptorBufferInfo bi{buffer, offset, size};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set;
  w.dstBinding = ShaderBindings::kUniforms;
  w.dstArrayElement = bankIndex;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w.pBufferInfo = &bi;
  ctx_->functions().vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

void DescriptorManager::WriteTexture(VkDescriptorSet set, uint32_t textureIndex, VkImageView view,
                                     VkImageLayout layout) {
  VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, layout};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set;
  w.dstBinding = ShaderBindings::kTextures;
  w.dstArrayElement = textureIndex;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo = &ii;
  ctx_->functions().vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

void DescriptorManager::WriteSampler(VkDescriptorSet set, uint32_t samplerIndex, VkSampler sampler) {
  VkDescriptorImageInfo ii{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = set;
  w.dstBinding = ShaderBindings::kSamplers;
  w.dstArrayElement = samplerIndex;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w.pImageInfo = &ii;
  ctx_->functions().vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
}

bool DescriptorManager::StageConstants(const void* data, uint32_t size, VkBuffer* outBuffer,
                                       VkDeviceSize* outOffset) {
  // Round the cursor up to a 256-byte uniform buffer offset alignment.
  const uint64_t align = 256;
  const uint64_t aligned = (uploadCursor_ + align - 1) / align * align;
  if (aligned + size > config_.uploadRingBytes) {
    return false;  // ring overflow; the caller should flush/resize.
  }
  std::memcpy(static_cast<uint8_t*>(uploadMap_) + aligned, data, size);
  if (outBuffer) *outBuffer = uploadBuffer_;
  if (outOffset) *outOffset = aligned;
  uploadCursor_ = aligned + size;
  return true;
}

void DescriptorManager::DestroyAll() {
  if (ctx_ == nullptr) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  for (auto& [key, sampler] : samplerCache_) {
    if (sampler) {
      fn.vkDestroySampler(device, sampler, nullptr);
    }
  }
  samplerCache_.clear();
  if (pool_) {
    fn.vkDestroyDescriptorPool(device, pool_, nullptr);
  }
  pool_ = VK_NULL_HANDLE;
  if (pipelineLayout_) {
    fn.vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
  }
  pipelineLayout_ = VK_NULL_HANDLE;
  if (shaderSetLayout_) {
    fn.vkDestroyDescriptorSetLayout(device, shaderSetLayout_, nullptr);
  }
  shaderSetLayout_ = VK_NULL_HANDLE;
  if (uploadBuffer_) {
    ctx_->FreeBuffer(uploadBuffer_, uploadAlloc_);
    uploadBuffer_ = VK_NULL_HANDLE;
    uploadMap_ = nullptr;
  }
}

}  // namespace fable2::renderer::vulkan
