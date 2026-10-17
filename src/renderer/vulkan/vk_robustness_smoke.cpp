// W11 — Cross-system robustness smoke test implementation.
#include "vk_robustness_smoke.h"

#include <cstdio>
#include <thread>
#include <vector>

#include "vk_context.h"

namespace fable2::renderer::vulkan {
namespace {

int gChecks = 0;
int gFailed = 0;

void Check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) {
    ++gFailed;
    std::printf("  FAIL: %s\n", what);
  } else {
    std::printf("  ok: %s\n", what);
  }
}

bool RunRobustnessSmoke() {
  std::printf("=== fable2 W11 cross-system robustness smoke test ===\n");

  // --- 1. Feature tier detection ---
  {
    VkContextConfig cfg;
    cfg.withPresentation = false;
    cfg.withValidation = false;
    std::string err;
    std::unique_ptr<VkContext> ctx;
    if (!VkContext::Create(cfg, ctx, err)) {
      std::printf("  SKIP: VkContext creation failed: %s\n", err.c_str());
      return false;
    }
    const FeatureTier tier = ctx->caps().Tier();
    std::printf("  Tier: %s\n", ctx->caps().TierName());
    Check(tier != FeatureTier::kUnsupported, "feature tier is supported");
    Check(ctx->caps().IsSufficient(nullptr), "capabilities are sufficient");
    // On RTX 3070, should be Tier 2 (Vulkan 1.2 + dynamic rendering + maintenance4).
    if (tier >= FeatureTier::kTier2_Vulkan12) {
      Check(ctx->caps().dynamicRendering, "Tier 2: dynamic rendering available");
      Check(ctx->caps().maintenance4, "Tier 2: maintenance4 available");
    } else if (tier >= FeatureTier::kTier1_Vulkan11) {
      Check(ctx->caps().timelineSemaphore, "Tier 1: timeline semaphore available");
    }
    ctx.reset();  // Clean shutdown (destructor order).
    Check(true, "clean shutdown (destructor order)");
  }

  // --- 2. Thread-safety: concurrent allocations ---
  {
    VkContextConfig cfg;
    cfg.withPresentation = false;
    cfg.withValidation = false;
    std::string err;
    std::unique_ptr<VkContext> ctx;
    if (!VkContext::Create(cfg, ctx, err)) {
      std::printf("  SKIP: VkContext creation failed: %s\n", err.c_str());
      return false;
    }
    const int kThreads = 4;
    const int kAllocsPerThread = 16;
    std::vector<std::thread> threads;
    std::vector<VkBuffer> buffers(kThreads * kAllocsPerThread);
    std::vector<VmaAllocation> allocs(kThreads * kAllocsPerThread);
    std::vector<bool> ok(kThreads * kAllocsPerThread, true);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kAllocsPerThread; ++i) {
          const int idx = t * kAllocsPerThread + i;
          std::string aErr;
          if (!ctx->AllocateBuffer(&buffers[idx], &allocs[idx], 4096,
                                   VMA_MEMORY_USAGE_GPU_ONLY,
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, nullptr, aErr)) {
            ok[idx] = false;
          }
        }
      });
    }
    for (auto& th : threads) th.join();
    bool allOk = true;
    for (int i = 0; i < kThreads * kAllocsPerThread; ++i) {
      if (!ok[i] || buffers[i] == VK_NULL_HANDLE) allOk = false;
    }
    Check(allOk, "concurrent allocations (4 threads x 16 buffers)");
    // Free all.
    for (int i = 0; i < kThreads * kAllocsPerThread; ++i) {
      if (allocs[i]) ctx->FreeBuffer(buffers[i], allocs[i]);
    }
    ctx.reset();
    Check(true, "clean shutdown after concurrent allocations");
  }

  // --- 3. Graceful failure: missing extensions ---
  {
    // Force a device name that doesn't exist — should fail gracefully.
    VkContextConfig cfg;
    cfg.withPresentation = false;
    cfg.withValidation = false;
    cfg.forcedDeviceName = "NoSuchGPU12345";
    std::string err;
    std::unique_ptr<VkContext> ctx;
    const bool result = VkContext::Create(cfg, ctx, err);
    Check(!result, "graceful failure: non-existent device name");
    Check(!err.empty(), "graceful failure: error message is set");
    Check(!ctx, "graceful failure: no context created");
  }

  // --- 4. Graceful failure: presentation required but unavailable ---
  {
    // Force presentation without a window — should fail gracefully (no surface).
    VkContextConfig cfg;
    cfg.withPresentation = true;
    cfg.withValidation = false;
    std::string err;
    std::unique_ptr<VkContext> ctx;
    const bool result = VkContext::Create(cfg, ctx, err);
    // On a headless system this may fail; on a system with a display it may succeed.
    // Either way, it must not crash.
    if (result) {
      Check(ctx->caps().presentationSupported, "presentation: swapchain support detected");
      ctx.reset();
    } else {
      Check(!err.empty(), "graceful failure: presentation error message");
    }
    Check(true, "graceful failure: presentation path does not crash");
  }

  // --- 5. Memory tracker + tier reporting ---
  {
    VkContextConfig cfg;
    cfg.withPresentation = false;
    cfg.withValidation = false;
    std::string err;
    std::unique_ptr<VkContext> ctx;
    if (!VkContext::Create(cfg, ctx, err)) {
      std::printf("  SKIP: VkContext creation failed: %s\n", err.c_str());
      return false;
    }
    // Allocate and free a few resources, verify memory tracker.
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    std::string aErr;
    Check(ctx->AllocateBuffer(&buf, &alloc, 65536, VMA_MEMORY_USAGE_GPU_ONLY,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, nullptr, aErr),
          "allocation for memory tracker test");
    Check(ctx->memory().TotalResidentBytes() >= 65536, "memory tracker tracks allocation");
    ctx->FreeBuffer(buf, alloc);
    Check(ctx->memory().TotalResidentBytes() == 0, "memory tracker clears on free");
    // Tier name is non-empty.
    Check(std::strlen(ctx->caps().TierName()) > 0, "tier name is non-empty");
    ctx.reset();
  }

  std::printf("=== fable2 W11 cross-system robustness smoke test: %s (%d checks, %d failed) ===\n",
              gFailed == 0 ? "PASS" : "FAIL", gChecks, gFailed);
  return gFailed == 0;
}

}  // namespace

bool RunRobustnessSmokeTest() {
  return RunRobustnessSmoke();
}

}  // namespace fable2::renderer::vulkan
