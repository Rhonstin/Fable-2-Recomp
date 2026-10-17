#pragma once

// W10: Performance profiler smoke test.
//
// A headless `fable2-vk-smoke --perf` run that verifies the FrameProfiler
// correctly tracks per-frame timing, draw/descriptor/pipeline call counts,
// and rolling statistics over multiple frames.

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

int RunPerfSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
