// W10 — Performance smoke test: per-frame arena, state batcher, dirty pages.
//
// Verifies:
//   - PerFrameArena: allocation, staging, flush, grow
//   - StateBatcher: dirty tracking, commit, skip
//   - DirtyPageTracker: register, mark dirty, check, clear, skip count
//   - Post-FX: FXAA + gamma fullscreen passes

#pragma once

namespace fable2::renderer::vulkan {
bool RunPerfSmokeTest();
}  // namespace fable2::renderer::vulkan
