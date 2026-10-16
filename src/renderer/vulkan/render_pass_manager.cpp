// render_pass_manager.cpp — W3: render passes & subpass design.
#include "render_pass_manager.h"

#include "vk_context.h"

#include <array>

namespace fable2::renderer::vulkan {

namespace {

// Choose an initial image layout for an attachment given its load op.
VkImageLayout InitialLayoutFor(VkAttachmentLoadOp loadOp, bool isColor) {
  if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
    return isColor ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                   : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  // CLEAR / DONT_CARE: the driver picks the initial contents.
  return VK_IMAGE_LAYOUT_UNDEFINED;
}

}  // namespace

RenderPassManager::RenderPassManager(VkContext* ctx) : ctx_(ctx) {}

RenderPassManager::~RenderPassManager() { DestroyAll(); }

PassLayout RenderPassManager::GetPass(const RenderPassKey& key) {
  auto it = passCache_.find(key);
  if (it != passCache_.end()) {
    ++passHits_;
    return it->second;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  // Build the attachment list. Layout:
  //   0: color (MSAA if colorSamples > 1)
  //   1: resolve (single-sample) — only when hasColorResolve
  //   last: depth/stencil — only when hasDepth
  std::array<VkAttachmentDescription, 3> attachments{};
  PassLayout layout{};
  uint32_t attCount = 0;

  // Color.
  VkAttachmentDescription& color = attachments[attCount];
  color.format = key.colorFormat;
  color.samples = key.colorSamples;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.flags = 0;
  if (key.hasColorResolve) {
    // MSAA color is transient: its result is resolved into attachment 1.
    color.loadOp = key.colorLoadOp;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  } else {
    color.loadOp = key.colorLoadOp;
    color.storeOp = key.colorStoreOp;
    color.initialLayout = InitialLayoutFor(key.colorLoadOp, true);
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  layout.colorAttachment = 0;
  ++attCount;

  // Resolve (single-sample) — index 1.
  if (key.hasColorResolve) {
    VkAttachmentDescription& res = attachments[attCount];
    res.format = key.colorFormat;
    res.samples = VK_SAMPLE_COUNT_1_BIT;
    res.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    res.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    res.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the resolve op writes it.
    res.storeOp = key.colorStoreOp;
    res.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    res.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    layout.resolveAttachment = 1;
    ++attCount;
  }

  // Depth/stencil — last index.
  if (key.hasDepth) {
    VkAttachmentDescription& ds = attachments[attCount];
    ds.format = key.depthFormat;
    ds.samples = key.depthSamples;
    ds.loadOp = key.depthLoadOp;
    ds.storeOp = key.depthStoreOp;
    ds.stencilLoadOp = key.depthLoadOp;
    ds.stencilStoreOp = key.depthStoreOp;
    ds.initialLayout = InitialLayoutFor(key.depthLoadOp, false);
    ds.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    layout.depthAttachment = static_cast<int>(attCount);
    ++attCount;
  }
  layout.attachmentCount = attCount;

  // Subpass 0: one color output, optional resolve, optional depth, optional
  // framebuffer fetch (color used as an input attachment — a feedback loop).
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolveRef{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{static_cast<uint32_t>(layout.depthAttachment),
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  // Framebuffer fetch: read the current value of the color attachment. For a
  // feedback loop the input layout matches the color layout.
  VkAttachmentReference fetchRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  if (key.hasColorResolve) {
    sub.pResolveAttachments = &resolveRef;
  }
  if (key.hasDepth) {
    sub.pDepthStencilAttachment = &depthRef;
  }
  if (key.framebufferFetch) {
    sub.inputAttachmentCount = 1;
    sub.pInputAttachments = &fetchRef;
  }

  // Explicit external<->subpass0 dependencies (safer than relying on the
  // implicit ones, and required for the input-attachment read to see prior
  // writes).
  VkSubpassDependency extToSub{};
  extToSub.srcSubpass = VK_SUBPASS_EXTERNAL;
  extToSub.dstSubpass = 0;
  extToSub.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  extToSub.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  extToSub.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  extToSub.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  VkSubpassDependency subToExt{};
  subToExt.srcSubpass = 0;
  subToExt.dstSubpass = VK_SUBPASS_EXTERNAL;
  subToExt.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  subToExt.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  subToExt.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT;
  subToExt.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  static const std::array<VkSubpassDependency, 2> kDeps{extToSub, subToExt};

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = attCount;
  rpci.pAttachments = attachments.data();
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 2;
  rpci.pDependencies = kDeps.data();

  VkRenderPass pass = VK_NULL_HANDLE;
  if (fn.vkCreateRenderPass(device, &rpci, nullptr, &pass) != VK_SUCCESS || pass == VK_NULL_HANDLE) {
    layout.pass = VK_NULL_HANDLE;  // caller detects the failure via pass==null.
    return layout;
  }
  layout.pass = pass;
  ++passCreates_;
  passCache_.emplace(key, layout);
  return layout;
}

VkFramebuffer RenderPassManager::GetFramebuffer(VkRenderPass pass, uint32_t width, uint32_t height,
                                                const std::vector<VkImageView>& views) {
  std::vector<uint64_t> viewHandles;
  viewHandles.reserve(views.size());
  for (VkImageView v : views) {
    viewHandles.push_back(reinterpret_cast<uint64_t>(v));
  }
  FbKey key{pass, width, height, std::move(viewHandles)};
  auto it = fbCache_.find(key);
  if (it != fbCache_.end()) {
    return it->second;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();

  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = pass;
  fci.attachmentCount = static_cast<uint32_t>(views.size());
  fci.pAttachments = views.data();
  fci.width = width;
  fci.height = height;
  fci.layers = 1;

  VkFramebuffer fb = VK_NULL_HANDLE;
  if (fn.vkCreateFramebuffer(device, &fci, nullptr, &fb) != VK_SUCCESS || fb == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;  // caller detects the failure via fb==null.
  }
  fbCache_.emplace(key, fb);
  return fb;
}

VkImageMemoryBarrier RenderPassManager::MakeBarrier(VkImage image, VkImageLayout oldLayout,
                                                    VkImageLayout newLayout, VkAccessFlags srcAccess,
                                                    VkAccessFlags dstAccess,
                                                    VkImageAspectFlags aspects, uint32_t mipLevels,
                                                    uint32_t layers) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.image = image;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange.aspectMask = aspects;
  b.subresourceRange.baseMipLevel = 0;
  b.subresourceRange.levelCount = mipLevels;
  b.subresourceRange.baseArrayLayer = 0;
  b.subresourceRange.layerCount = layers;
  return b;
}

void RenderPassManager::DestroyAll() {
  if (ctx_ == nullptr) {
    return;
  }
  Functions& fn = ctx_->functions();
  const VkDevice device = ctx_->device();
  for (auto& [key, fb] : fbCache_) {
    if (fb) {
      fn.vkDestroyFramebuffer(device, fb, nullptr);
    }
  }
  fbCache_.clear();
  for (auto& [key, layout] : passCache_) {
    if (layout.pass) {
      fn.vkDestroyRenderPass(device, layout.pass, nullptr);
    }
  }
  passCache_.clear();
}

}  // namespace fable2::renderer::vulkan
