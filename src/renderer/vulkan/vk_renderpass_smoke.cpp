// vk_renderpass_smoke.cpp — W3: render passes & subpass design self-test.
#include "vk_renderpass_smoke.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "render_pass_manager.h"

namespace fable2::renderer::vulkan {

namespace {

// MSAA helper: 1/2/4/8/16 -> human string of supported bits.
std::string SampleBits(VkSampleCountFlags flags) {
  std::string s;
  const auto add = [&](VkSampleCountFlagBits b, const char* n) {
    if (flags & b) {
      if (!s.empty()) s += '|';
      s += n;
    }
  };
  add(VK_SAMPLE_COUNT_1_BIT, "1");
  add(VK_SAMPLE_COUNT_2_BIT, "2");
  add(VK_SAMPLE_COUNT_4_BIT, "4");
  add(VK_SAMPLE_COUNT_8_BIT, "8");
  add(VK_SAMPLE_COUNT_16_BIT, "16");
  return s.empty() ? "(none)" : s;
}

// Render targets backing a single render pass: one image per attachment in
// the pass's attachment order (color, [resolve], [depth]).
struct Rts {
  std::vector<VkImage> images;
  std::vector<VmaAllocation> allocs;
  std::vector<VkImageView> views;

  bool Create(VkContext* ctx, const RenderPassKey& key, uint32_t w, uint32_t h,
              std::string* err) {
    auto& f = ctx->functions();
    const auto makeOne = [&](VkFormat fmt, VkSampleCountFlagBits samples,
                             VkImageUsageFlags usage) -> bool {
      VkImageCreateInfo cbi{};
      cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      cbi.imageType = VK_IMAGE_TYPE_2D;
      cbi.format = fmt;
      cbi.extent = {w, h, 1};
      cbi.mipLevels = 1;
      cbi.arrayLayers = 1;
      cbi.samples = samples;
      cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
      cbi.usage = usage;
      cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImage img;
      VmaAllocation alloc;
      if (!ctx->AllocateImage(&img, &alloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "rpsmoke-rt", *err)) {
        return false;
      }
      images.push_back(img);
      allocs.push_back(alloc);
      VkImageViewCreateInfo ivc{};
      ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      ivc.image = img;
      ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ivc.format = fmt;
      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      if (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT ||
          fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
          fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      }
      if (fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
          fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
      }
      ivc.subresourceRange.aspectMask = aspect;
      ivc.subresourceRange.baseMipLevel = 0;
      ivc.subresourceRange.levelCount = 1;
      ivc.subresourceRange.baseArrayLayer = 0;
      ivc.subresourceRange.layerCount = 1;
      VkImageView view;
      if (f.vkCreateImageView(ctx->device(), &ivc, nullptr, &view) != VK_SUCCESS) {
        *err = "vkCreateImageView failed";
        return false;
      }
      views.push_back(view);
      return true;
    };

    VkImageUsageFlags colorUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!makeOne(key.colorFormat, key.colorSamples, colorUsage)) {
      return false;
    }
    if (key.hasColorResolve &&
        !makeOne(key.colorFormat, VK_SAMPLE_COUNT_1_BIT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
      return false;
    }
    if (key.hasDepth && !makeOne(key.depthFormat, key.depthSamples,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      return false;
    }
    return true;
  }

  void Destroy(VkContext* ctx) {
    auto& f = ctx->functions();
    for (VkImageView v : views) {
      if (v) f.vkDestroyImageView(ctx->device(), v, nullptr);
    }
    for (std::size_t i = 0; i < images.size(); ++i) {
      ctx->FreeImage(images[i], allocs[i]);
    }
    images.clear();
    allocs.clear();
    views.clear();
  }
};

struct Case {
  const char* name;
  RenderPassKey key;
};

// Allocate a plain off-screen color image (no view) for the blit pipeline test.
bool MakePipelineImage(VkContext* ctx, uint32_t w, uint32_t h, VkImage* outImg,
                       VmaAllocation* outAlloc, std::string* err) {
  VkImageCreateInfo cbi{};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
  cbi.extent = {w, h, 1};
  cbi.mipLevels = 1;
  cbi.arrayLayers = 1;
  cbi.samples = VK_SAMPLE_COUNT_1_BIT;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return ctx->AllocateImage(outImg, outAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY,
                            "rpsmoke-pipeline", *err);
}

// Demonstrate the guest-frame model end-to-end with off-screen RTTs:
//   scene render (simulated clear) -> GPU downscale (blit) -> composite (blit).
// This is the "scene -> downscale/resolve -> composite to swapchain" pipeline
// expressed with blit/copy/resolve over off-screen targets (the RTT case).
bool PipelineCompositeStage(VkContext& ctx, const char* what, std::string& report) {
  auto& f = ctx.functions();
  const uint32_t FULL = 256, HALF = 128;
  VkImage scene, mid, out;
  VmaAllocation sceneA, midA, outA;
  std::string err;
  if (!MakePipelineImage(&ctx, FULL, FULL, &scene, &sceneA, &err) ||
      !MakePipelineImage(&ctx, HALF, HALF, &mid, &midA, &err) ||
      !MakePipelineImage(&ctx, FULL, FULL, &out, &outA, &err)) {
    report += std::string(what) + ": alloc failed: " + err + "\n";
    return false;
  }

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pcpi{};
  pcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pcpi.queueFamilyIndex = static_cast<uint32_t>(ctx.caps().graphicsFamily);
  f.vkCreateCommandPool(ctx.device(), &pcpi, nullptr, &pool);
  VkCommandBufferAllocateInfo abci{};
  abci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  abci.commandPool = pool;
  abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  abci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(ctx.device(), &abci, &cb);
  VkCommandBufferBeginInfo bbi{};
  bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(cb, &bbi);

  const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  const auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                           VkAccessFlags srcAcc, VkAccessFlags dstAcc) {
    VkImageMemoryBarrier b = RenderPassManager::MakeBarrier(img, oldL, newL, srcAcc, dstAcc,
                                                            VK_IMAGE_ASPECT_COLOR_BIT);
    f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  const auto blit = [&](VkImage src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw,
                        uint32_t dh) {
    VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit blit{};
    blit.srcSubresource = sub;
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(sw), static_cast<int32_t>(sh), 1};
    blit.dstSubresource = sub;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(dw), static_cast<int32_t>(dh), 1};
    f.vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
  };

  // Stage 1: scene (simulated by a clear to a known color).
  barrier(scene, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0);
  const VkClearColorValue clearColor = {0.25f, 0.5f, 0.75f, 1.0f};
  f.vkCmdClearColorImage(cb, scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &colorRange);
  // Stage 2: downscale scene (FULL) -> mid (HALF).
  barrier(scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  barrier(mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0);
  blit(scene, FULL, FULL, mid, HALF, HALF);
  // Stage 3: composite mid (HALF) -> out (FULL, the "swapchain").
  barrier(mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  barrier(out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0);
  blit(mid, HALF, HALF, out, FULL, FULL);
  // Make the composited output sampleable (ready for the next stage / present).
  barrier(out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

  f.vkEndCommandBuffer(cb);
  VkFence fence;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(ctx.device(), &fci, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult submitRes = f.vkQueueSubmit(ctx.graphicsQueue(), 1, &si, fence);
  const VkResult waitRes = f.vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, 3000000000ull);
  const bool ok = (submitRes == VK_SUCCESS && waitRes == VK_SUCCESS);

  f.vkDestroyFence(ctx.device(), fence, nullptr);
  f.vkFreeCommandBuffers(ctx.device(), pool, 1, &cb);
  f.vkDestroyCommandPool(ctx.device(), pool, nullptr);
  ctx.FreeImage(scene, sceneA);
  ctx.FreeImage(mid, midA);
  ctx.FreeImage(out, outA);

  report += std::string(what) +
            (ok ? std::string(
                     ": OK (scene clear -> 256->128 downscale -> 128->256 composite)")
                : (": FAILED (submit=" + std::to_string(submitRes) +
                   " wait=" + std::to_string(waitRes) + ")")) +
            "\n";
  return ok;
}

}  // namespace

int RunRenderPassSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  auto line = [&](bool ok, const std::string& what) {
    report += std::string(ok ? "[ok]   " : "[FAIL] ") + what + "\n";
  };

  VkContextConfig cfg = config;
  cfg.withPresentation = false;  // headless
  std::string err;
  std::unique_ptr<VkContext> ctx;
  if (!VkContext::Create(cfg, ctx, err)) {
    line(false, "VkContext::Create: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VkContext created");

  const auto& caps = ctx->caps();
  line(true, "MSAA framebuffer sample counts: color=" + SampleBits(caps.msaaColorSamples) +
                 " depth=" + SampleBits(caps.msaaDepthSamples) +
                 " stencil=" + SampleBits(caps.msaaStencilSamples));
  line(caps.msaaColorSamples & VK_SAMPLE_COUNT_4_BIT,
       "device supports 4x MSAA color (needed for the MSAA+resolve case)");

  RenderPassManager rpm(ctx.get());

  // The small fixed set of render passes the guest-frame model needs.
  std::vector<Case> cases;
  {
    // 1. color-only, 1x, CLEAR.
    Case a{};
    a.name = "color1x";
    a.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(a);
    // 2. color-only, 1x, LOAD (must differ from #1 -> load op is a key field).
    Case b{};
    b.name = "color1xLoad";
    b.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    cases.push_back(b);
    // 3. color + depth, 1x, CLEAR.
    Case c{};
    c.name = "color+depth1x";
    c.key.hasDepth = true;
    c.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    c.key.depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(c);
    // 4. MSAA 4x color + in-pass resolve to 1x, CLEAR.
    Case d{};
    d.name = "msaa4+resolve";
    d.key.colorSamples = VK_SAMPLE_COUNT_4_BIT;
    d.key.hasColorResolve = true;
    d.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(d);
    // 5. MSAA 4x color + resolve + MSAA 4x depth, CLEAR.
    Case e{};
    e.name = "msaa4+resolve+depth";
    e.key.colorSamples = VK_SAMPLE_COUNT_4_BIT;
    e.key.hasColorResolve = true;
    e.key.hasDepth = true;
    e.key.depthSamples = VK_SAMPLE_COUNT_4_BIT;
    e.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    e.key.depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(e);
    // 6. framebuffer fetch, 1x, CLEAR.
    Case fcase{};
    fcase.name = "framebuffer-fetch";
    fcase.key.framebufferFetch = true;
    fcase.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(fcase);
    // 7. framebuffer fetch + depth, 1x, CLEAR.
    Case g{};
    g.name = "framebuffer-fetch+depth";
    g.key.framebufferFetch = true;
    g.key.hasDepth = true;
    g.key.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    g.key.depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cases.push_back(g);
  }

  // Create all the passes.
  std::vector<PassLayout> layouts(cases.size());
  bool allPasses = true;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    layouts[i] = rpm.GetPass(cases[i].key);
    const bool ok = layouts[i].pass != VK_NULL_HANDLE;
    allPasses &= ok;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s pass created (att=%u color=%d resolve=%d depth=%d)",
                  cases[i].name, layouts[i].attachmentCount, layouts[i].colorAttachment,
                  layouts[i].resolveAttachment, layouts[i].depthAttachment);
    line(ok, buf);
  }

  // Cache behavior: same key -> same handle + a hit; different key -> different handle.
  const PassLayout sameAgain = rpm.GetPass(cases[0].key);
  line(sameAgain.pass == layouts[0].pass,
       "cache reuse: re-fetching color1x returns the same handle");
  const PassLayout diff = rpm.GetPass(cases[1].key);
  line(diff.pass != layouts[0].pass,
       "cache distinct: color1xLoad (LOAD) differs from color1x (CLEAR)");
  line(rpm.passCount() == cases.size(),
       "unique pass count == " + std::to_string(cases.size()) + " (got " +
           std::to_string(rpm.passCount()) + ")");

  // Allocate RTs, build framebuffers, and record every pass into one command
  // buffer, then submit + wait.
  const uint32_t W = 256, H = 256;
  auto& f = ctx->functions();
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pcpi{};
  pcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pcpi.queueFamilyIndex = static_cast<uint32_t>(caps.graphicsFamily);
  f.vkCreateCommandPool(ctx->device(), &pcpi, nullptr, &pool);
  VkCommandBufferAllocateInfo abci{};
  abci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  abci.commandPool = pool;
  abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  abci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(ctx->device(), &abci, &cb);
  VkCommandBufferBeginInfo bbi{};
  bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(cb, &bbi);

  bool allFbs = true;
  std::vector<Rts> rts(cases.size());
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (layouts[i].pass == VK_NULL_HANDLE) continue;
    std::string rtsErr;
    if (!rts[i].Create(ctx.get(), cases[i].key, W, H, &rtsErr)) {
      line(false, std::string(cases[i].name) + " RT alloc: " + rtsErr);
      allFbs = false;
      continue;
    }
    VkFramebuffer fb = rpm.GetFramebuffer(layouts[i].pass, W, H, rts[i].views);
    if (fb == VK_NULL_HANDLE) {
      line(false, std::string(cases[i].name) + " framebuffer create failed");
      allFbs = false;
      continue;
    }
    // Record: begin the render pass (clearing color/depth), end it.
    VkClearColorValue cc{0.25f, 0.5f, 0.75f, 1.0f};
    VkClearDepthStencilValue ds{0.0f, 0};
    std::array<VkClearValue, 2> clears;
    clears[0].color = cc;
    clears[1].depthStencil = ds;
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = layouts[i].pass;
    rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {W, H}};
    rbi.clearValueCount = static_cast<uint32_t>(clears.size());
    rbi.pClearValues = clears.data();
    f.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdEndRenderPass(cb);
    line(true, std::string(cases[i].name) + " recorded (fb over " +
                   std::to_string(rts[i].views.size()) + " attachments)");
  }
  f.vkEndCommandBuffer(cb);

  VkFence fence;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(ctx->device(), &fci, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult submitRes = f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
  const VkResult waitRes = f.vkWaitForFences(ctx->device(), 1, &fence, VK_TRUE, 3000000000ull);
  const bool gpuOk = (submitRes == VK_SUCCESS && waitRes == VK_SUCCESS);
  line(gpuOk, "GPU executed all render passes (submit=" + std::to_string(submitRes) +
                  " wait=" + std::to_string(waitRes) + ")");

  // Layout-transition barrier sanity (W3 "correct layout transitions").
  if (!rts[0].images.empty()) {
    VkImageMemoryBarrier b = RenderPassManager::MakeBarrier(
        rts[0].images[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    line(b.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             b.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             b.image == rts[0].images[0],
         "layout transition barrier (COLOR_ATTACHMENT -> SHADER_READ_ONLY) built");
  }

  // Teardown.
  f.vkDestroyFence(ctx->device(), fence, nullptr);
  f.vkFreeCommandBuffers(ctx->device(), pool, 1, &cb);
  f.vkDestroyCommandPool(ctx->device(), pool, nullptr);
  for (auto& r : rts) r.Destroy(ctx.get());
  rpm.DestroyAll();
  line(rpm.passCount() == 0 && rpm.framebufferCount() == 0,
       "teardown: all render passes + framebuffers destroyed");

  line(true, "render passes: " + std::to_string(rpm.passCount()) +
                 " (created " + std::to_string(rpm.passCreates()) + ", cache hits " +
                 std::to_string(rpm.passHits()) + ")");

  // W3 pipeline model: scene -> downscale/resolve -> composite over RTTs.
  const bool pipelineOk = PipelineCompositeStage(*ctx, "downscale/resolve/composite", report);

  report += "\n" + ctx->memory().Describe() + "\n";
  std::printf("%s", report.c_str());
  if (!reportFile.empty()) {
    std::ofstream f2(reportFile);
    f2 << report;
  }

  const bool pass = allPasses && allFbs && gpuOk && pipelineOk;
  std::printf("\n=== fable2 render-pass smoke test (W3): %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
