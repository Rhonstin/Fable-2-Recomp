#pragma once

// W9: shader translation & pipeline assembly self-test.
//
// A headless `fable2-vk-smoke --shader` run that proves the multi-stage
// pipeline assembly works end-to-end:
//   * VS+FS pipeline (baseline),
//   * VS+HS+DS+FS tessellated pipeline,
//   * pipeline cache reuse (same key → same handle),
//   * pixel readback verification.

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

int RunShaderSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
