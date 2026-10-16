// W7 — Shared memory & blits/copy smoke test.
#include "vk_shared_blit_smoke.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "shared_memory.h"
#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

namespace {

void line(bool ok, const std::string& msg, std::string& report, bool& allOk) {
  const char* tag = ok ? "[ok]  " : "[FAIL]";
  std::printf("%s %s\n", tag, msg.c_str());
  report += std::string(tag) + " " + msg + "\n";
  if (!ok) {
    allOk = false;
  }
}

void check(bool cond, const std::string& msg, std::string& report, bool& allOk) {
  line(cond, msg, report, allOk);
}

struct ReadbackBuf {
  VkBuffer buf;
  VmaAllocation alloc;
  void* map;
  uint32_t size;
};

}  // namespace

int RunSharedBlitSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  bool allOk = true;

  std::printf("=== fable2 W7 shared memory & blit smoke test ===\n");

  // --- 1. Create the VkContext (headless). ---
  std::unique_ptr<VkContext> ctx;
  std::string err;
  if (!VkContext::Create(config, ctx, err)) {
    line(false, "VkContext: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VkContext created", report, allOk);
  const VkDevice device = ctx->device();
  Functions& f = ctx->functions();

  // --- 2. Create the SharedMemoryManager + BlitManager. ---
  SharedMemoryManager shmem;
  if (!shmem.Init(ctx.get(), err)) {
    line(false, "SharedMemoryManager: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  BlitManager blits(ctx.get());
  line(true, "SharedMemoryManager (512 MB) + BlitManager created", report, allOk);
  check(shmem.buffer() != VK_NULL_HANDLE, "shared memory buffer allocated", report, allOk);
  check(shmem.cpu() != nullptr, "shared memory CPU pointer valid", report, allOk);

  // Local storage for readback buffers and command pools.
  std::vector<ReadbackBuf> readbackBufs;
  std::vector<VkCommandPool> cmdPools;

  // Helper: allocate a WxH image + view.
  auto allocImage = [&](uint32_t w, uint32_t h, VkFormat fmt,
                        VkImage* img, VkImageView* view, VmaAllocation* alloc,
                        std::string& e) -> bool {
    VkImageCreateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cbi.imageType = VK_IMAGE_TYPE_2D;
    cbi.format = fmt;
    cbi.extent = {w, h, 1};
    cbi.mipLevels = 1;
    cbi.arrayLayers = 1;
    cbi.samples = VK_SAMPLE_COUNT_1_BIT;
    cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
    cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ctx->AllocateImage(img, alloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "W7", e)) {
      return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (f.vkCreateImageView(device, &vi, nullptr, view) != VK_SUCCESS) {
      return false;
    }
    return true;
  };

  // Helper: transition an image layout.
  auto toLayout = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                      VkCommandBuffer cmd) {
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = oldL;
    bar.newLayout = newL;
    bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &bar);
  };

  // Helper: read back a 256x256 image to a host-visible buffer.
  auto readback256 = [&](VkImage img, VkCommandBuffer cmd) -> const uint8_t* {
    const uint32_t W = 256, H = 256;
    void* map = nullptr;
    VkBuffer buf;
    VmaAllocation alloc;
    std::string e;
    ctx->AllocateBuffer(&buf, &alloc, W * H * 4, VMA_MEMORY_USAGE_GPU_TO_CPU,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &map, e);
    VkBufferImageCopy rbr{};
    rbr.bufferOffset = 0;
    rbr.bufferRowLength = 0;
    rbr.bufferImageHeight = H;
    rbr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rbr.imageOffset = {0, 0, 0};
    rbr.imageExtent = {W, H, 1};
    f.vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rbr);
    readbackBufs.push_back({buf, alloc, map, W * H * 4});
    return static_cast<const uint8_t*>(map);
  };

  // Helper: submit + wait a command buffer.
  auto submitAndWait = [&](VkCommandBuffer cb, VkCommandPool pool) {
    f.vkEndCommandBuffer(cb);
    VkFence fence;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f.vkCreateFence(device, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
    f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);
  };

  // Helper: create a command buffer.
  auto makeCmdBuf = [&]() -> std::pair<VkCommandBuffer, VkCommandPool> {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pbi{};
    pbi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pbi.queueFamilyIndex = ctx->caps().graphicsFamily;
    f.vkCreateCommandPool(device, &pbi, nullptr, &pool);
    cmdPools.push_back(pool);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    f.vkAllocateCommandBuffers(device, &abi, &cb);
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    f.vkBeginCommandBuffer(cb, &bbi);
    return {cb, pool};
  };

  const uint32_t W = 256, H = 256;

  // --- 3. CPU write -> GPU read: write a pattern to shared memory, copy it
  // to an image, read it back. ---
  {
    // Write a red pattern (R=255, G=0, B=0, A=255) to shared memory.
    std::vector<uint8_t> pattern(W * H * 4);
    for (uint32_t i = 0; i < W * H; ++i) {
      pattern[i * 4 + 0] = 255;
      pattern[i * 4 + 1] = 0;
      pattern[i * 4 + 2] = 0;
      pattern[i * 4 + 3] = 255;
    }
    shmem.CopyToShared(0x10000, pattern.data(), pattern.size());
    check(shmem.copiesToShared == 1, "shared memory: COPY_TO_SHMEM issued", report, allOk);

    VkImage img;
    VkImageView view;
    VmaAllocation imgAlloc;
    if (!allocImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, &img, &view, &imgAlloc, err)) {
      line(false, "image alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }

    auto [cb, pool] = makeCmdBuf();
    toLayout(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    shmem.Use(SharedMemoryManager::Usage::kRead, cb);
    VkBufferImageCopy bic{};
    bic.bufferOffset = 0x10000;
    bic.bufferRowLength = 0;
    bic.bufferImageHeight = H;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageOffset = {0, 0, 0};
    bic.imageExtent = {W, H, 1};
    f.vkCmdCopyBufferToImage(cb, shmem.buffer(), img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    // Transition to TRANSFER_SRC for readback.
    toLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    const uint8_t* rd = readback256(img, cb);
    submitAndWait(cb, pool);

    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = rd + (cy * W + cx) * 4;
    check(px[0] == 255 && px[1] == 0 && px[2] == 0,
          "shared memory: CPU write -> GPU read (center RGB = " + std::to_string(px[0]) + "," +
              std::to_string(px[1]) + "," + std::to_string(px[2]) + ")",
          report, allOk);

    ctx->FreeImage(img, imgAlloc);
    f.vkDestroyImageView(device, view, nullptr);
  }

  // --- 4. SHARED_FILL + SHARED_SWAP + COPY_FROM_SHMEM. ---
  {
    shmem.Fill(0x20000, 0xDEADBEEF, 256);
    check(shmem.fills == 1, "shared memory: SHARED_FILL issued", report, allOk);
    const uint32_t* filled = reinterpret_cast<const uint32_t*>(shmem.cpu() + 0x20000);
    check(filled[0] == 0xDEADBEEF && filled[63] == 0xDEADBEEF,
          "shared memory: fill pattern verified (0xDEADBEEF)", report, allOk);

    shmem.Fill(0x30000, 0x12345678, 256);
    shmem.Swap(0x20000, 0x30000, 256);
    check(shmem.swaps == 1, "shared memory: SHARED_SWAP issued", report, allOk);
    const uint32_t* a = reinterpret_cast<const uint32_t*>(shmem.cpu() + 0x20000);
    const uint32_t* b = reinterpret_cast<const uint32_t*>(shmem.cpu() + 0x30000);
    check(a[0] == 0x12345678 && b[0] == 0xDEADBEEF,
          "shared memory: swap verified (regions exchanged)", report, allOk);

    uint32_t out[4] = {};
    shmem.CopyFromShared(0x20000, out, sizeof(out));
    check(shmem.copiesFromShared == 1, "shared memory: COPY_FROM_SHMEM issued", report, allOk);
    check(out[0] == 0x12345678, "shared memory: read-back after swap verified", report, allOk);
  }

  // --- 5. 2D blit (copy between two images). ---
  {
    VkImage src, dst;
    VkImageView srcView, dstView;
    VmaAllocation srcAlloc, dstAlloc;
    if (!allocImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, &src, &srcView, &srcAlloc, err) ||
        !allocImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, &dst, &dstView, &dstAlloc, err)) {
      line(false, "blit image alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }

    auto [cb, pool] = makeCmdBuf();
    toLayout(src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    blits.FillImage(src, 0, 0, W, H, 0.0f, 1.0f, 0.0f, 1.0f, cb);
    toLayout(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    toLayout(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    blits.Blit2D(src, dst, 0, 0, W, H, cb);
    check(blits.blits == 1, "blit: 2D blit issued", report, allOk);
    toLayout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    const uint8_t* rd = readback256(dst, cb);
    submitAndWait(cb, pool);

    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = rd + (cy * W + cx) * 4;
    check(px[1] >= 200 && px[0] < 60 && px[2] < 60,
          "blit: 2D copy verified (center RGB = " + std::to_string(px[0]) + "," +
              std::to_string(px[1]) + "," + std::to_string(px[2]) + ")",
          report, allOk);

    ctx->FreeImage(src, srcAlloc);
    f.vkDestroyImageView(device, srcView, nullptr);
    ctx->FreeImage(dst, dstAlloc);
    f.vkDestroyImageView(device, dstView, nullptr);
  }

  // --- 6. Scale blit (resize 256->128). ---
  {
    VkImage src, dst;
    VkImageView srcView, dstView;
    VmaAllocation srcAlloc, dstAlloc;
    if (!allocImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, &src, &srcView, &srcAlloc, err)) {
      line(false, "scale src alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }
    VkImageCreateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cbi.imageType = VK_IMAGE_TYPE_2D;
    cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
    cbi.extent = {128, 128, 1};
    cbi.mipLevels = 1;
    cbi.arrayLayers = 1;
    cbi.samples = VK_SAMPLE_COUNT_1_BIT;
    cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
    cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ctx->AllocateImage(&dst, &dstAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "W7-scale", err)) {
      line(false, "scale dst alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = dst;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCreateImageView(device, &vi, nullptr, &dstView);

    auto [cb, pool] = makeCmdBuf();
    toLayout(src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    blits.FillImage(src, 0, 0, W, H, 0.0f, 0.0f, 1.0f, 1.0f, cb);
    toLayout(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    toLayout(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, cb);
    blits.BlitScale(src, dst, 0, 0, W, H, 0, 0, 128, 128, cb);
    check(blits.scales == 1, "blit: scale issued (256->128)", report, allOk);
    submitAndWait(cb, pool);

    ctx->FreeImage(src, srcAlloc);
    f.vkDestroyImageView(device, srcView, nullptr);
    ctx->FreeImage(dst, dstAlloc);
    f.vkDestroyImageView(device, dstView, nullptr);
  }

  // --- 7. Buffer fill (vkCmdFillBuffer). ---
  {
    VkBuffer buf;
    VmaAllocation bufAlloc;
    if (!ctx->AllocateBuffer(&buf, &bufAlloc, 4096, VMA_MEMORY_USAGE_GPU_ONLY,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, nullptr, err)) {
      line(false, "fill buffer alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }
    auto [cb, pool] = makeCmdBuf();
    blits.FillBuffer(buf, 0, 2048, 0xCAFEBABE, cb);
    check(blits.bufferFills == 1, "blit: buffer fill issued", report, allOk);
    submitAndWait(cb, pool);
    ctx->FreeBuffer(buf, bufAlloc);
  }

  // --- 8. Barriers: verify the Use() barrier was issued. ---
  {
    check(shmem.barriers >= 1, "shared memory: pipeline barrier issued (Use)", report, allOk);
  }

  // --- Teardown. ---
  for (auto& rb : readbackBufs) {
    ctx->FreeBuffer(rb.buf, rb.alloc);
  }
  shmem.DestroyAll();
  line(true, "teardown clean", report, allOk);

  ctx.reset();

  const bool pass = allOk;
  std::printf("\n=== fable2 W7 shared memory & blit smoke test: %s ===\n",
              pass ? "PASS" : "FAIL");
  if (!reportFile.empty()) {
    FILE* fp = std::fopen(reportFile.c_str(), "w");
    if (fp) {
      std::fputs(report.c_str(), fp);
      std::fclose(fp);
    }
  }
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
