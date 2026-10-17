#pragma once

// W10: Frame-level performance profiler for the custom Vulkan renderer.
//
// Tracks per-frame timing and aggregates call counts from the renderer
// modules (DrawDispatcher, PipelineManager, TextureCache, RenderTargetCache,
// DescriptorManager). Lightweight: no per-packet heap allocation, just
// integer counters and a high-resolution timestamp.

#include <cstdint>
#include <cstdio>
#include <chrono>

namespace fable2::renderer::vulkan {

// Per-frame statistics. Reset each frame via FrameProfiler::BeginFrame().
struct FrameStats {
  // Timing (milliseconds).
  double cpuEncodeMs = 0.0;  // CPU command encoding time (BeginFrame to EndFrame).
  double guestFrameMs = 0.0; // Guest-reported frame time (set externally).

  // Draw/descriptor/pipeline call counts.
  uint32_t drawCalls = 0;
  uint32_t drawIndexedCalls = 0;
  uint32_t descriptorSetWrites = 0;
  uint32_t pipelineBindings = 0;
  uint32_t pipelineCreates = 0;
  uint32_t pipelineCacheHits = 0;
  uint32_t multiPipelineCreates = 0;
  uint32_t multiPipelineCacheHits = 0;

  // Texture cache.
  uint32_t textureCacheHits = 0;
  uint32_t textureCacheMisses = 0;
  uint32_t textureUploads = 0;
  uint64_t textureUploadBytes = 0;
  uint32_t formatConversions = 0;
  uint64_t formatConversionBytes = 0;

  // Render-target cache.
  uint32_t rtCacheHits = 0;
  uint32_t rtCacheMisses = 0;
  uint32_t msaaResolves = 0;
  uint32_t msaaResolveSkips = 0;  // 1x RTs that skip resolve.

  // Command buffer / submission.
  uint32_t commandBuffersRecorded = 0;
  uint32_t queueSubmits = 0;
  uint32_t fenceWaits = 0;

  // Blit/copy operations.
  uint32_t blitOps = 0;
  uint32_t bufferCopies = 0;
  uint32_t imageFills = 0;

  // Memory.
  uint64_t memoryPeakBytes = 0;
  uint64_t memoryFrameBytes = 0;

  // Reset all counters (preserves timing for the in-progress frame).
  void Reset() {
    *this = FrameStats{};
  }
};

// Rolling statistics over a window of frames.
struct RollingStats {
  uint32_t frameCount = 0;
  double totalCpuMs = 0.0;
  double totalGuestMs = 0.0;
  uint64_t totalDrawCalls = 0;
  uint64_t totalDrawIndexedCalls = 0;
  uint64_t totalDescriptorWrites = 0;
  uint64_t totalPipelineBindings = 0;
  double avgCpuMs = 0.0;
  double avgGuestMs = 0.0;
  double avgDrawsPerFrame = 0.0;
  double avgDescriptorWritesPerFrame = 0.0;
  double avgPipelineBindingsPerFrame = 0.0;

  void AddFrame(const FrameStats& s) {
    ++frameCount;
    totalCpuMs += s.cpuEncodeMs;
    totalGuestMs += s.guestFrameMs;
    totalDrawCalls += s.drawCalls;
    totalDrawIndexedCalls += s.drawIndexedCalls;
    totalDescriptorWrites += s.descriptorSetWrites;
    totalPipelineBindings += s.pipelineBindings;
    if (frameCount > 0) {
      avgCpuMs = totalCpuMs / frameCount;
      avgGuestMs = totalGuestMs / frameCount;
      avgDrawsPerFrame = static_cast<double>(totalDrawCalls + totalDrawIndexedCalls) / frameCount;
      avgDescriptorWritesPerFrame = static_cast<double>(totalDescriptorWrites) / frameCount;
      avgPipelineBindingsPerFrame = static_cast<double>(totalPipelineBindings) / frameCount;
    }
  }

  void Reset() {
    *this = RollingStats{};
  }
};

class FrameProfiler {
 public:
  FrameProfiler() = default;

  // Call at the start of each frame. Resets the per-frame counters and
  // starts the CPU encode timer.
  void BeginFrame() {
    current_.Reset();
    frameStart_ = std::chrono::high_resolution_clock::now();
  }

  // Call at the end of each frame. Stops the timer, computes CPU encode time,
  // and stores the frame stats for rolling aggregation.
  void EndFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    current_.cpuEncodeMs =
        std::chrono::duration<double, std::milli>(now - frameStart_).count();
    last_ = current_;
    rolling_.AddFrame(current_);
    ++frameCount_;
  }

  // Set the guest-reported frame time (e.g., from the Xenos vblank pacing).
  void SetGuestFrameTime(double ms) { current_.guestFrameMs = ms; }

  // Access the current (in-progress) frame stats for counter accumulation.
  FrameStats& Current() { return current_; }
  const FrameStats& Current() const { return current_; }

  // Access the completed previous frame's stats.
  const FrameStats& LastFrame() const { return last_; }

  // Rolling statistics over all frames since construction (or Reset()).
  const RollingStats& Rolling() const { return rolling_; }

  // Total frames profiled.
  uint32_t FrameCount() const { return frameCount_; }

  // Reset all counters and rolling stats.
  void Reset() {
    current_.Reset();
    last_ = FrameStats{};
    rolling_.Reset();
    frameCount_ = 0;
  }

  // Print a human-readable report to stdout.
  void Report(const char* title = "FrameProfiler") const {
    std::printf("\n=== %s ===\n", title);
    std::printf("  Frames: %u\n", frameCount_);
    if (frameCount_ > 0) {
      std::printf("  CPU encode: avg %.3f ms  (total %.1f ms)\n",
                  rolling_.avgCpuMs, rolling_.totalCpuMs);
      std::printf("  Guest frame: avg %.3f ms  (total %.1f ms)\n",
                  rolling_.avgGuestMs, rolling_.totalGuestMs);
      std::printf("  Draws/frame: avg %.2f  (total %llu non-indexed + %llu indexed)\n",
                  rolling_.avgDrawsPerFrame,
                  (unsigned long long)rolling_.totalDrawCalls,
                  (unsigned long long)rolling_.totalDrawIndexedCalls);
      std::printf("  Descriptor writes/frame: avg %.2f\n",
                  rolling_.avgDescriptorWritesPerFrame);
      std::printf("  Pipeline bindings/frame: avg %.2f\n",
                  rolling_.avgPipelineBindingsPerFrame);
    }
    if (frameCount_ > 1) {
      // Print the last frame's detailed stats.
      const FrameStats& s = last_;
      std::printf("\n  Last frame detail:\n");
      std::printf("    CPU encode: %.3f ms\n", s.cpuEncodeMs);
      std::printf("    Draws: %u non-indexed, %u indexed\n", s.drawCalls, s.drawIndexedCalls);
      std::printf("    Descriptor writes: %u\n", s.descriptorSetWrites);
      std::printf("    Pipeline creates: %u (hits: %u, multi creates: %u, multi hits: %u)\n",
                  s.pipelineCreates, s.pipelineCacheHits,
                  s.multiPipelineCreates, s.multiPipelineCacheHits);
      std::printf("    Texture cache: %u hits, %u misses, %u uploads (%llu bytes)\n",
                  s.textureCacheHits, s.textureCacheMisses,
                  s.textureUploads, (unsigned long long)s.textureUploadBytes);
      std::printf("    RT cache: %u hits, %u misses, %u MSAA resolves, %u skips\n",
                  s.rtCacheHits, s.rtCacheMisses, s.msaaResolves, s.msaaResolveSkips);
      std::printf("    Blits: %u, buffer copies: %u, image fills: %u\n",
                  s.blitOps, s.bufferCopies, s.imageFills);
      std::printf("    Cmd buffers: %u, submits: %u, fence waits: %u\n",
                  s.commandBuffersRecorded, s.queueSubmits, s.fenceWaits);
    }
    std::printf("===\n");
  }

 private:
  FrameStats current_{};
  FrameStats last_{};
  RollingStats rolling_{};
  uint32_t frameCount_ = 0;
  std::chrono::high_resolution_clock::time_point frameStart_{};
};

}  // namespace fable2::renderer::vulkan
