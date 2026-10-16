#pragma once

// W8: primitives, vertex/index buffers & draw dispatch self-test.
//
// A headless `fable2-vk-smoke --draw` run that proves the DrawDispatcher
// works end-to-end:
//   * topology mapping (Xenos → Vulkan),
//   * non-indexed draw (triangle list),
//   * indexed draw (16-bit and 32-bit),
//   * triangle strip, line list, point list,
//   * viewport + scissor setup,
//   * pixel readback verification.

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

int RunDrawSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
