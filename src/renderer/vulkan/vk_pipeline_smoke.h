// vk_pipeline_smoke.h — W4: descriptor sets & pipeline state self-test.
//
// Headless: creates a VkContext (headless) + DescriptorManager + PipelineManager
// + RenderPassManager, compiles two small GLSL shaders to SPIR-V (via glslang),
// builds a small set of cached graphics pipelines, allocates descriptor sets
// from a per-frame pool, stages a constant buffer + bindless texture + sampler,
// and records + submits a batch of draws. It verifies:
//   - steady-state pipeline reuse (same state -> same VkPipeline + cache hit);
//   - sampler dedup (identical configs share one VkSampler);
//   - descriptor sets come from the per-frame pool (zero per-draw allocation);
//   - the constant buffer upload ring stages cbuffer data;
//   - the persistent (on-disk) pipeline cache: after save + reload, pipelines
//     hit the cache.
//
// Namespace: fable2::renderer::vulkan
#pragma once

#include <string>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Returns a process exit code (0 = pass). Prints a report to stdout and, if
// `reportFile` is non-empty, also writes it to that path.
int RunPipelineSmokeTest(const VkContextConfig& config, const std::string& reportFile);

}  // namespace fable2::renderer::vulkan
