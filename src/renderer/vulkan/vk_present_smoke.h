#pragma once
//
// fable2 :: renderer :: vulkan :: present smoke test (W2)
//
// A standalone, windowed self-test for the FramePresenter (workstream W2 —
// frame sync & presentation). It opens a real Win32 window, creates a
// VkSurfaceKHR, builds a FramePresenter (swapchain + N-in-flight sync ring),
// and runs a present loop that:
//   * presents a hue-cycling clear color every frame (proves frames flow),
//   * exercises vkAcquireNextImageKHR + vkQueuePresentKHR with finite
//     timeouts (skip-present-not-block semantics),
//   * (optionally) resizes the window mid-run to exercise swapchain recreate,
//   * (optionally) runs vsync-on (FIFO, capped to display) and vsync-off
//     (IMMEDIATE/MAILBOX, uncapped) to show the present-rate difference.
//
// It reports present rate (fps), frames presented, dropped/skipped/out-of-date
// counts, the chosen present mode, format and extent, then returns 0 on
// success.
//
#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

struct PresentSmokeConfig {
  double seconds = 3.0;      // how long to run the present loop
  uint32_t width = 960;
  uint32_t height = 600;
  bool vsync = true;         // true -> FIFO (capped), false -> IMMEDIATE/MAILBOX
  bool allowImmediate = false;
  bool allowMailbox = true;
  bool doResize = false;     // resize the window mid-run to test recreate
  double capFps = 0.0;       // optional frame-rate cap (0 = none); vblank pacing
};

// Runs the windowed present self-test. Returns 0 on success, non-zero on
// failure. Prints a detailed report to stdout and (optionally) to
// `reportFile`.
int RunPresentSmokeTest(const VkContextConfig& ctxConfig, const PresentSmokeConfig& cfg,
                        const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
