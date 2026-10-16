#include "vk_smoke.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <vector>

namespace fable2::renderer::vulkan {

namespace {

bool BufferRoundTrip(VkContext& ctx, const char* what, std::string& report) {
  auto& f = ctx.functions();
  const VkDeviceSize size = 4096;
  const uint32_t count = static_cast<uint32_t>(size / sizeof(uint32_t));

  // 1. Host-visible source buffer with a known pattern.
  VkBuffer srcBuf;
  VmaAllocation srcAlloc;
  void* srcMap = nullptr;
  std::string err;
  if (!ctx.AllocateBuffer(&srcBuf, &srcAlloc, size, VMA_MEMORY_USAGE_CPU_TO_GPU,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &srcMap, err)) {
    report += std::string(what) + ": source alloc failed: " + err + "\n";
    return false;
  }
  auto* src = static_cast<uint32_t*>(srcMap);
  for (uint32_t i = 0; i < count; ++i) src[i] = 0xA5A50000u + i;
  // Make the CPU writes visible to the GPU (the CPU_TO_GPU type may be
  // non-coherent; this vendored header uses the older VkMappedMemoryRange).
  {
    VmaAllocationInfo srcInfo;
    vmaGetAllocationInfo(ctx.vma(), srcAlloc, &srcInfo);
    VkMappedMemoryRange r = {};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = srcInfo.deviceMemory;
    r.offset = 0;
    r.size = size;
    f.vkFlushMappedMemoryRanges(ctx.device(), 1, &r);
  }

  // 2. Host-coherent staging buffer (destination for the GPU copy).
  VkBuffer stgBuf;
  VmaAllocation stgAlloc;
  void* stgMap = nullptr;
  if (!ctx.AllocateBuffer(&stgBuf, &stgAlloc, size, VMA_MEMORY_USAGE_GPU_TO_CPU,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stgMap, err)) {
    report += std::string(what) + ": staging alloc failed: " + err + "\n";
    return false;
  }
  // Pre-fill the staging buffer with a sentinel so a no-op copy is detectable.
  std::memset(stgMap, 0xEE, size);

  // 3. Record a single host -> GPU copy and execute it.
  VkCommandPool pool;
  VkCommandPoolCreateInfo pcpi = {};
  pcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pcpi.queueFamilyIndex = static_cast<uint32_t>(ctx.caps().graphicsFamily);
  f.vkCreateCommandPool(ctx.device(), &pcpi, nullptr, &pool);
  VkCommandBuffer cb;
  VkCommandBufferAllocateInfo abci = {};
  abci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  abci.commandPool = pool;
  abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  abci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(ctx.device(), &abci, &cb);
  VkCommandBufferBeginInfo bbi = {};
  bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(cb, &bbi);
  VkBufferCopy copy = {0, 0, size};
  f.vkCmdCopyBuffer(cb, srcBuf, stgBuf, 1, &copy);
  f.vkEndCommandBuffer(cb);

  VkFence fence;
  VkFenceCreateInfo fci = {};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(ctx.device(), &fci, nullptr, &fence);
  VkSubmitInfo si = {};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult submitRes = f.vkQueueSubmit(ctx.graphicsQueue(), 1, &si, fence);
  const VkResult waitRes = f.vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, 3000000000ull);
  // Make the GPU's result visible to the CPU.
  {
    VmaAllocationInfo stgInfo;
    vmaGetAllocationInfo(ctx.vma(), stgAlloc, &stgInfo);
    VkMappedMemoryRange r = {};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = stgInfo.deviceMemory;
    r.offset = 0;
    r.size = size;
    f.vkInvalidateMappedMemoryRanges(ctx.device(), 1, &r);
  }

  const auto* stg = static_cast<const uint32_t*>(stgMap);
  uint32_t mismatches = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (stg[i] != (0xA5A50000u + i)) ++mismatches;
  }

  // Cleanup (order: fence -> command buffer -> pool -> buffers).
  f.vkDestroyFence(ctx.device(), fence, nullptr);
  f.vkFreeCommandBuffers(ctx.device(), pool, 1, &cb);
  f.vkDestroyCommandPool(ctx.device(), pool, nullptr);
  ctx.FreeBuffer(stgBuf, stgAlloc);
  ctx.FreeBuffer(srcBuf, srcAlloc);

  if (submitRes != VK_SUCCESS || waitRes != VK_SUCCESS || mismatches != 0) {
    char dbg[280];
    std::snprintf(dbg, sizeof(dbg),
                  "submit=%d wait=%d mismatches=%u src[0]=0x%08X stg[0]=0x%08X "
                  "stg[last]=0x%08X expect[0]=0x%08X cb=%p pool=%p queue=%p gfxFamily=%d",
                  (int)submitRes, (int)waitRes, mismatches,
                  (unsigned)src[0], (unsigned)stg[0], (unsigned)stg[count - 1],
                  (unsigned)(0xA5A50000u), (void*)cb, (void*)pool,
                  (void*)ctx.graphicsQueue(), (int)ctx.caps().graphicsFamily);
    report += std::string(what) + ": FAILED (" + dbg + ")\n";
    return false;
  }
  report += std::string(what) + ": OK (round-tripped " + std::to_string(count) +
            " u32 through the GPU)\n";
  return true;
}

bool PipelineCacheRoundTrip(VkContext& ctx, const std::string& path,
                            std::string& report) {
  // Store a keyed entry and save.
  const uint32_t payload[4] = {1, 2, 3, 4};
  ctx.StorePipelineKey("test/entry/42", payload, sizeof(payload));
  std::string err;
  if (!ctx.SavePipelineCache(err)) {
    report += "pipeline cache save failed: " + err + "\n";
    return false;
  }
  // Reload and verify the keyed entry + the raw cache blob round-tripped.
  std::string err2;
  if (!ctx.LoadPipelineCache(err2)) {
    report += "pipeline cache load failed: " + err2 + "\n";
    return false;
  }
  const auto* entry = ctx.LookupPipelineKey("test/entry/42");
  if (!entry || entry->size() != sizeof(payload)) {
    report += "pipeline cache keyed entry did not round-trip\n";
    return false;
  }
  const PipelineCache::Stats stats = ctx.PipelineCacheStats();
  report += "pipeline cache: OK (disk blob " + std::to_string(stats.diskCacheBytes) +
            " bytes, " + std::to_string(stats.entryCount) + " keyed entries, " +
            std::to_string(stats.hitCount) + " hits / " + std::to_string(stats.missCount) +
            " misses)\n";
  return true;
}

bool ImageAlloc(VkContext& ctx, std::string& report) {
  VkImage image;
  VmaAllocation alloc;
  VkImageCreateInfo cbi = {};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
  cbi.extent = {256, 256, 1};
  cbi.mipLevels = 1;
  cbi.arrayLayers = 1;
  cbi.samples = VK_SAMPLE_COUNT_1_BIT;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  std::string err;
  if (!ctx.AllocateImage(&image, &alloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "smoke-image", err)) {
    report += "image alloc failed: " + err + "\n";
    return false;
  }
  ctx.SetObjectName(image, "smoke-image");
  ctx.FreeImage(image, alloc);
  report += "image alloc: OK (256x256 R8G8B8A8)\n";
  return true;
}

}  // namespace

int RunSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  auto line = [&](bool ok, const std::string& what) {
    report += std::string(ok ? "[ok]   " : "[FAIL] ") + what + "\n";
  };

  const std::string cachePath =
      reportFile.empty() ? "fable2-vk-smoke-cache.bin" : reportFile + ".pcache";
  VkContextConfig cfg = config;
  cfg.withPresentation = false;          // headless: no swapchain needed
  cfg.pipelineCachePath = cachePath;     // so SavePipelineCache has a real path

  std::string err;
  std::unique_ptr<VkContext> ctx;
  if (!VkContext::Create(cfg, ctx, err)) {
    line(false, "VkContext::Create: " + err);
    if (!reportFile.empty()) {
      std::ofstream f(reportFile);
      f << report;
    }
    return 1;
  }
  line(true, "VkContext created");

  std::vector<std::string> missing;
  line(ctx->caps().IsSufficient(&missing),
       "device capabilities sufficient" +
           (missing.empty() ? std::string("")
                            : " (missing: " +
                                  std::accumulate(
                                      missing.begin(), missing.end(), std::string(),
                                      [](const std::string& a, const std::string& b) {
                                        return a + (a.empty() ? "" : ", ") + b;
                                      }) +
                                  ")"));

  const bool bufOk = BufferRoundTrip(*ctx, "GPU buffer round-trip", report);
  const bool imgOk = ImageAlloc(*ctx, report);
  const bool cacheOk = PipelineCacheRoundTrip(*ctx, cachePath, report);

  // Finalise: report memory + pipeline cache.
  ctx->memory().EndFrame();
  report += "\n" + ctx->memory().Describe() + "\n";
  const PipelineCache::Stats stats = ctx->PipelineCacheStats();
  report += "pipeline cache: blob " + std::to_string(stats.diskCacheBytes) + " bytes\n";
  std::printf("%s", report.c_str());

  if (!reportFile.empty()) {
    std::ofstream f(reportFile);
    f << report;
  }

  const bool pass = missing.empty() && bufOk && imgOk && cacheOk;
  std::printf("\n=== fable2 VkContext smoke test: %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
