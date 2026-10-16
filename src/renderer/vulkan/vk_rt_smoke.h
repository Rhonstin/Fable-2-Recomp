#pragma once

// W6: the guest render-target cache self-test.
//
// A headless `fable2-vk-smoke --rt` run that proves the RenderTargetCache
// works end-to-end on a real device:
//   * color + depth RT allocation (1x and 4x MSAA),
//   * the Xenos -> Vulkan color/depth format mapping,
//   * MSAA resolve to the single-sample guest surface,
//   * guest RT content preservation across "frames" (LOAD),
//   * framebuffer reuse (the W3 RenderPassManager keys by RT set + render pass),
//   * a GPU render + readback of the single-sample surface (render-to-texture),
//   * reference-counting + LRU budget eviction,
//   * external-image adoption (the scratch texture at 0x12704000).

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Runs the W6 render-target cache smoke test. Returns 0 on pass, 1 on fail.
int RunRenderTargetSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
