#pragma once

// Fable2VulkanCommandProcessor
//
// The guest -> Vulkan command encoder for the Fable 2 custom renderer.
//
// It derives from the SDK's Vulkan command processor so the full, correct
// Xenos command-packet pipeline works out of the box (register writes, draw
// dispatch, copy/blit, render targets, shared memory, swap/present). For the
// first increment it is a *thin seam*: every method is inherited, and the
// overrides below exist to prove the seam is live and to hook the
// performance-program instrumentation (see plans/custom-vulkan-renderer.md,
// Workstream 10).
//
// The rewrite strategy (plan: "Open questions / risks") is to progressively
// replace the SDK's costliest Vulkan classes with custom implementations.
// Those replacements land here (and in the fable2/vulkan/ cache classes) while
// this class stays the single object the graphics system constructs.

#include <atomic>
#include <cstdint>

#include <rex/graphics/vulkan/command_processor.h>

namespace fable2::renderer {

class Fable2VulkanCommandProcessor : public rex::graphics::vulkan::VulkanCommandProcessor {
 public:
  Fable2VulkanCommandProcessor(rex::graphics::vulkan::VulkanGraphicsSystem* graphics_system,
                               rex::system::KernelState* kernel_state)
      : rex::graphics::vulkan::VulkanCommandProcessor(graphics_system, kernel_state) {}

  // Per-frame present seam. Delegates to the base (which does the actual
  // resolve/composite/present) and records how many guest frames have been
  // presented, so the profiler hook (Workstream 10) has a live counter.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override {
    presented_frames_.fetch_add(1, std::memory_order_relaxed);
    rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(frontbuffer_ptr, frontbuffer_width,
                                                             frontbuffer_height);
  }

  // Number of guest frames presented since startup. Exposed for the
  // diagnostics/profiling overlay.
  uint64_t presented_frame_count() const { return presented_frames_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> presented_frames_{0};
};

}  // namespace fable2::renderer
