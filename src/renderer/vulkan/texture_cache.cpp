// W5: the guest texture cache (image manager) on VkContext.
#include "texture_cache.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace fable2::renderer::vulkan {

namespace {

// Xenos TextureFormat ordinals (mirror of xenos::TextureFormat in the SDK).
constexpr uint32_t kFmt_8 = 2;
constexpr uint32_t kFmt_1_5_5_5 = 3;
constexpr uint32_t kFmt_5_6_5 = 4;
constexpr uint32_t kFmt_8_8_8_8 = 6;
constexpr uint32_t kFmt_2_10_10_10 = 7;
constexpr uint32_t kFmt_8_8 = 10;
constexpr uint32_t kFmt_10_11_11 = 16;
constexpr uint32_t kFmt_11_11_10 = 17;
constexpr uint32_t kFmt_DXT1 = 18;
constexpr uint32_t kFmt_DXT2_3 = 19;
constexpr uint32_t kFmt_DXT4_5 = 20;
constexpr uint32_t kFmt_16 = 24;
constexpr uint32_t kFmt_16_16 = 25;
constexpr uint32_t kFmt_16_FLOAT = 30;
constexpr uint32_t kFmt_32_FLOAT = 36;
constexpr uint32_t kFmt_32_32_FLOAT = 37;
constexpr uint32_t kFmt_32_32_32_32_FLOAT = 38;

struct FormatEntry {
  uint32_t xenos;
  VkFormat vk;
  bool native;
  uint32_t blockW, blockH;
};

// The common Fable 2 texture formats. `native` means the guest byte layout maps
// 1:1 to the VkFormat (at most a component swizzle the load shader handles), so
// the guest memory can be uploaded directly without per-pixel conversion.
const FormatEntry kFormats[] = {
    {kFmt_8, VK_FORMAT_R8_UNORM, true, 1, 1},
    {kFmt_1_5_5_5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, false, 1, 1},
    {kFmt_5_6_5, VK_FORMAT_R5G6B5_UNORM_PACK16, false, 1, 1},
    {kFmt_8_8_8_8, VK_FORMAT_R8G8B8A8_UNORM, true, 1, 1},
    {kFmt_2_10_10_10, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true, 1, 1},
    {kFmt_8_8, VK_FORMAT_R8G8_UNORM, true, 1, 1},
    {kFmt_10_11_11, VK_FORMAT_R16G16B16A16_UNORM, false, 1, 1},
    {kFmt_11_11_10, VK_FORMAT_R16G16B16A16_UNORM, false, 1, 1},
    {kFmt_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true, 4, 4},
    {kFmt_DXT2_3, VK_FORMAT_BC2_UNORM_BLOCK, true, 4, 4},
    {kFmt_DXT4_5, VK_FORMAT_BC3_UNORM_BLOCK, true, 4, 4},
    {kFmt_16, VK_FORMAT_R16_UNORM, true, 1, 1},
    {kFmt_16_16, VK_FORMAT_R16G16_UNORM, true, 1, 1},
    {kFmt_16_FLOAT, VK_FORMAT_R16_SFLOAT, true, 1, 1},
    {kFmt_32_FLOAT, VK_FORMAT_R32_SFLOAT, true, 1, 1},
    {kFmt_32_32_FLOAT, VK_FORMAT_R32G32_SFLOAT, true, 1, 1},
    {kFmt_32_32_32_32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, true, 1, 1},
};

bool IsBlockCompressed(VkFormat f) {
  return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC2_UNORM_BLOCK ||
         f == VK_FORMAT_BC3_UNORM_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK ||
         f == VK_FORMAT_BC5_UNORM_BLOCK;
}

}  // namespace

bool XenosToVulkanFormat(uint32_t xenosFormat, XenosFormatInfo& out) {
  for (const auto& e : kFormats) {
    if (e.xenos == xenosFormat) {
      out.vkFormat = e.vk;
      out.native = e.native;
      out.blockW = e.blockW;
      out.blockH = e.blockH;
      return true;
    }
  }
  return false;
}

TextureCache::~TextureCache() { DestroyAll(); }

GuestTexture* TextureCache::Find(const GuestTextureKey& key) const {
  auto it = textures_.find(key);
  return it == textures_.end() ? nullptr : const_cast<GuestTexture*>(&it->second);
}

GuestTexture* TextureCache::CreateOrUpdate(const GuestTextureKey& key, uint32_t frame) {
  if (auto* existing = Find(key)) {
    existing->lastUsedFrame = frame;
    return existing;
  }
  return CreateTexture(key, frame);
}

GuestTexture* TextureCache::Acquire(const GuestTextureKey& key, uint32_t frame) {
  GuestTexture* tex = CreateOrUpdate(key, frame);
  if (tex) {
    tex->refCount++;
  }
  return tex;
}

GuestTexture* TextureCache::CreateTexture(const GuestTextureKey& key, uint32_t frame) {
  if (!ctx_) {
    return nullptr;
  }
  XenosFormatInfo fmt;
  if (!XenosToVulkanFormat(key.format, fmt) || fmt.vkFormat == VK_FORMAT_UNDEFINED) {
    return nullptr;
  }

  auto slot = textures_.emplace(key, GuestTexture{}).first;
  GuestTexture& tex = slot->second;
  tex.key = key;
  tex.format = fmt.vkFormat;
  tex.native = fmt.native;
  tex.width = key.width;
  tex.height = key.height;
  tex.mipLevels = key.mipLevels > 0 ? key.mipLevels : 1;
  tex.refCount = 0;
  tex.lastUsedFrame = frame;
  tex.lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageCreateInfo cbi = {};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = fmt.vkFormat;
  cbi.extent = {key.width, key.height, 1};
  cbi.mipLevels = tex.mipLevels;
  cbi.arrayLayers = 1;
  cbi.samples = VK_SAMPLE_COUNT_1_BIT;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  std::string err;
  if (!ctx_->AllocateImage(&tex.image, &tex.allocation, cbi, VMA_MEMORY_USAGE_GPU_ONLY,
                           "guest texture", err)) {
    textures_.erase(slot);
    return nullptr;
  }
  // Track the real allocation size for eviction accounting.
  VmaAllocationInfo ainfo = {};
  vmaGetAllocationInfo(ctx_->vma(), tex.allocation, &ainfo);
  tex.bytes = ainfo.size;
  totalBytes_ += tex.bytes;

  VkImageViewCreateInfo vbi = {};
  vbi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vbi.image = tex.image;
  vbi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vbi.format = fmt.vkFormat;
  vbi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vbi.subresourceRange.baseMipLevel = 0;
  vbi.subresourceRange.levelCount = tex.mipLevels;
  vbi.subresourceRange.layerCount = 1;
  if (ctx_->functions().vkCreateImageView(ctx_->device(), &vbi, nullptr, &tex.view) !=
      VK_SUCCESS) {
    ctx_->FreeImage(tex.image, tex.allocation);
    tex.image = VK_NULL_HANDLE;
    tex.allocation = VK_NULL_HANDLE;
    textures_.erase(slot);
    return nullptr;
  }
  return &tex;
}

void TextureCache::FreeTexture(GuestTexture* tex) {
  if (!tex || !ctx_) {
    return;
  }
  Functions& fn = ctx_->functions();
  if (tex->view != VK_NULL_HANDLE) {
    fn.vkDestroyImageView(ctx_->device(), tex->view, nullptr);
    tex->view = VK_NULL_HANDLE;
  }
  if (tex->image != VK_NULL_HANDLE) {
    ctx_->FreeImage(tex->image, tex->allocation);
    totalBytes_ = (totalBytes_ >= tex->bytes) ? (totalBytes_ - tex->bytes) : 0;
  }
  tex->image = VK_NULL_HANDLE;
  tex->allocation = VK_NULL_HANDLE;
  tex->bytes = 0;
}

uint64_t TextureCache::Release(GuestTexture* tex, uint32_t frame) {
  if (!tex) {
    return 0;
  }
  if (tex->refCount > 0) {
    tex->refCount--;
  }
  if (tex->refCount == 0 && !tex->external) {
    auto it = textures_.find(tex->key);
    if (it != textures_.end()) {
      const uint64_t freed = it->second.bytes;
      FreeTexture(&it->second);
      textures_.erase(it);
      evicted++;
      return freed;
    }
  }
  return 0;
}

uint32_t TextureCache::EvictToBudget(uint64_t budgetBytes) {
  uint32_t evictedThisCall = 0;
  while (totalBytes_ > budgetBytes && !textures_.empty()) {
    GuestTexture* lru = nullptr;
    for (auto& [k, t] : textures_) {
      if (t.refCount == 0 && !t.external &&
          (lru == nullptr || t.lastUsedFrame < lru->lastUsedFrame)) {
        lru = &t;
      }
    }
    if (!lru) {
      break;
    }
    FreeTexture(lru);
    textures_.erase(textures_.find(lru->key));
    evicted++;
    evictedThisCall++;
  }
  return evictedThisCall;
}

bool TextureCache::EnsureUploadBuffer(uint32_t size, std::string& err) {
  if (uploadBuffer_ != VK_NULL_HANDLE && size <= uploadCapacity_) {
    return true;
  }
  if (uploadBuffer_ != VK_NULL_HANDLE) {
    ctx_->FreeBuffer(uploadBuffer_, uploadAlloc_);
    uploadBuffer_ = VK_NULL_HANDLE;
    uploadAlloc_ = VK_NULL_HANDLE;
    uploadMap_ = nullptr;
  }
  const uint32_t capacity = std::max<uint32_t>(size, 256 * 1024);
  void* map = nullptr;
  if (!ctx_->AllocateBuffer(&uploadBuffer_, &uploadAlloc_, capacity, VMA_MEMORY_USAGE_CPU_TO_GPU,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &map, err)) {
    return false;
  }
  uploadMap_ = map;
  uploadCapacity_ = capacity;
  return true;
}

bool TextureCache::UploadLevel0(GuestTexture* tex, const void* cpuData, uint32_t dataSize,
                                VkCommandBuffer cmd, bool sourceHostVisible) {
  if (!tex || !ctx_ || !cmd || !cpuData || dataSize == 0) {
    return false;
  }
  std::string err;
  if (!EnsureUploadBuffer(dataSize, err)) {
    return false;
  }
  Functions& fn = ctx_->functions();
  if (sourceHostVisible) {
    uploadsHostVisible++;
  } else {
    uploadsStaged++;
  }

  // 1) Copy the guest (CPU) bytes into the persistent host-visible upload
  //    buffer. For a host-visible guest source this is the "alias/native" fast
  //    path (a straight copy of the mapped guest memory); for a device-local
  //    source the full renderer would GPU-copy instead, but the image write is
  //    the same.
  std::memcpy(uploadMap_, cpuData, dataSize);
  {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(ctx_->vma(), uploadAlloc_, &info);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = info.deviceMemory;
    r.offset = 0;
    r.size = dataSize;
    fn.vkFlushMappedMemoryRanges(ctx_->device(), 1, &r);
  }

  // 2) Transition the image level 0 to TRANSFER_DST_OPTIMAL.
  VkImageMemoryBarrier b = {};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.oldLayout = tex->lastLayout;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.image = tex->image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  // 3) Barrier the upload buffer to TRANSFER_SRC.
  VkBufferMemoryBarrier bb = {};
  bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  bb.buffer = uploadBuffer_;
  bb.offset = 0;
  bb.size = dataSize;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                          nullptr, 1, &bb, 1, &b);
  tex->lastLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

  // 4) Copy the upload buffer into the image level 0.
  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;  // tightly packed
  region.bufferImageHeight = tex->height;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {tex->width, tex->height, 1};
  fn.vkCmdCopyBufferToImage(cmd, uploadBuffer_, tex->image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  return true;
}

void TextureCache::GenerateMips(GuestTexture* tex, VkCommandBuffer cmd) {
  if (!tex || !ctx_ || !cmd || tex->mipLevels <= 1) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkImageLayout opt = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  // Block-compressed images can only be blit with nearest filtering.
  const VkFilter filter = IsBlockCompressed(tex->format) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  for (uint32_t level = 0; level + 1 < tex->mipLevels; level++) {
    const uint32_t dstW = std::max<uint32_t>(tex->width >> (level + 1), 1);
    const uint32_t dstH = std::max<uint32_t>(tex->height >> (level + 1), 1);

    // src level -> TRANSFER_SRC.
    VkImageMemoryBarrier srcB = {};
    srcB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcB.srcAccessMask = level == 0 ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    srcB.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcB.oldLayout = level == 0 ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : opt;
    srcB.newLayout = opt;
    srcB.image = tex->image;
    srcB.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            0, nullptr, 0, nullptr, 1, &srcB);

    // dst level -> TRANSFER_DST.
    VkImageMemoryBarrier dstB = {};
    dstB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstB.srcAccessMask = 0;
    dstB.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstB.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstB.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstB.image = tex->image;
    dstB.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 1, 0, 1};
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            0, nullptr, 0, nullptr, 1, &dstB);

    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {int32_t(tex->width >> level), int32_t(tex->height >> level), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {int32_t(dstW), int32_t(dstH), 1};
    fn.vkCmdBlitImage(cmd, tex->image, opt, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      &blit, filter);
  }

  // Transition the whole range to SHADER_READ_ONLY_OPTIMAL.
  VkImageMemoryBarrier b = {};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = opt;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.image = tex->image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipLevels, 0, 1};
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  tex->lastLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  tex->mipsGenerated = true;
}

void TextureCache::SetLayout(GuestTexture* tex, VkImageLayout layout, VkCommandBuffer cmd) {
  if (!tex || !ctx_ || !cmd) {
    return;
  }
  Functions& fn = ctx_->functions();
  VkImageMemoryBarrier b = {};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT
                                                                       : 0;
  b.oldLayout = tex->lastLayout;
  b.newLayout = layout;
  b.image = tex->image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipLevels, 0, 1};
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  tex->lastLayout = layout;
}

VkDescriptorImageInfo TextureCache::MakeSampleInfo(const GuestTexture* tex, VkSampler sampler,
                                                   VkImageLayout layout) {
  VkDescriptorImageInfo ii = {};
  ii.sampler = sampler;
  ii.imageView = tex ? tex->view : VK_NULL_HANDLE;
  ii.imageLayout = layout;
  return ii;
}

GuestTexture* TextureCache::AdoptExternal(const GuestTextureKey& key, VkImage image,
                                          VkImageView view, VkFormat format, uint32_t frame) {
  auto slot = textures_.emplace(key, GuestTexture{}).first;
  GuestTexture& tex = slot->second;
  tex.key = key;
  tex.image = image;
  tex.view = view;
  tex.format = format;
  tex.external = true;
  tex.width = key.width;
  tex.height = key.height;
  tex.mipLevels = key.mipLevels > 0 ? key.mipLevels : 1;
  tex.refCount = 0;
  tex.lastUsedFrame = frame;
  tex.lastLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  return &tex;
}

void TextureCache::DestroyAll() {
  for (auto& [k, tex] : textures_) {
    if (!tex.external) {
      FreeTexture(&tex);
    }
  }
  textures_.clear();
  totalBytes_ = 0;
  if (uploadBuffer_ != VK_NULL_HANDLE) {
    ctx_->FreeBuffer(uploadBuffer_, uploadAlloc_);
    uploadBuffer_ = VK_NULL_HANDLE;
    uploadAlloc_ = VK_NULL_HANDLE;
    uploadMap_ = nullptr;
  }
}

}  // namespace fable2::renderer::vulkan
