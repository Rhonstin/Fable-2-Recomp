// W11 — Cross-system robustness smoke test.
//
// Verifies:
//   - Feature tier detection (Tier 2 on RTX 3070)
//   - Thread-safety of VkContext allocation (concurrent alloc/free)
//   - Clean shutdown (destructor order, no crash)
//   - Graceful failure (missing extensions don't crash)

#pragma once

namespace fable2::renderer::vulkan {
bool RunRobustnessSmokeTest();
}  // namespace fable2::renderer::vulkan
