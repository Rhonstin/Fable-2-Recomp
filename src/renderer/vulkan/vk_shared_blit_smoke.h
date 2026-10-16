#pragma once

// W7: shared memory & blits/copy self-test.
//
// A headless `fable2-vk-smoke --shared` run that proves the
// SharedMemoryManager and BlitManager work end-to-end:
//   * shared memory allocation (512 MB host-visible coherent buffer),
//   * CPU write -> GPU read (copy to image + readback),
//   * SHARED_FILL and SHARED_SWAP operations,
//   * 2D blit (copy between two images),
//   * scale blit (resize),
//   * image fill (vkCmdClearColorImage),
//   * buffer fill (vkCmdFillBuffer),
//   * pipeline barriers between shared memory and texture use.

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Runs the W7 shared memory & blit smoke test. Returns 0 on pass, 1 on fail.
int RunSharedBlitSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
