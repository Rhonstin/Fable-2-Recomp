// vk_renderpass_smoke.h — W3: render passes & subpass design self-test.
//
// Headless: creates a VkContext (no presentation), a RenderPassManager, and a
// small fixed set of render passes covering the guest-frame stages:
//   color-only, color+depth, MSAA color + in-pass resolve, MSAA + depth, and
//   the framebuffer-fetch (Xenos FETCH) feedback-loop variant. It verifies the
//   cache (same key -> same handle + hit; different key -> different handle),
//   creates the backing render-target images + framebuffers, records every
//   pass into a command buffer, and submits + waits.
//
// Namespace: fable2::renderer::vulkan
#pragma once

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Returns a process exit code (0 = pass). Prints a report to stdout and, if
// `reportFile` is non-empty, also writes it to that path.
int RunRenderPassSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
