#pragma once

// W5: the guest texture cache self-test.
//
// Proves on real hardware (headless, no window) that the TextureCache:
//   * maps Xenos guest formats to the correct VkFormat and flags native vs.
//     conversion (the CPU->GPU conversion lever),
//   * reference-counts textures and evicts them (freeing image memory),
//   * LRU-evicts to a byte budget,
//   * GPU-generates mip levels via vkCmdBlitImage (including block-compressed),
//   * transitions UNDEFINED -> SHADER_READ_ONLY,
//   * takes the host-visible fast path vs. the device-local staging path,
//   * composes a sampler view (VkDescriptorImageInfo) from a cached image view
//     + a canonical sampler,
//   * actually samples a cached guest texture to an off-screen RTT and reads a
//     pixel back (end-to-end proof),
//   * adopts an external image (decoded video frame).
//
// Returns 0 on PASS, 1 on FAIL.

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

int RunTextureSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
