#pragma once
//
// fable2 :: renderer :: vulkan :: smoke test
//
// A standalone, on-GPU self-test for the VkContext foundation (W1). It creates
// a real instance + device, does a host<->device buffer round-trip through the
// graphics queue, allocates an image, and exercises the on-disk pipeline cache
// round-trip, then reports a PASS/FAIL summary and the memory-cache stats.
//
#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Returns 0 on success, non-zero on failure. Prints a detailed report to
// stdout and (optionally) to `reportFile`.
int RunSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
