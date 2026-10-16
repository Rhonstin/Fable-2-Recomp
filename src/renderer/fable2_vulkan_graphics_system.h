#pragma once

// Fable2VulkanGraphicsSystem
//
// The top-level IGraphicsSystem implementation for the Fable 2 custom Vulkan
// renderer. It derives from the SDK's Vulkan graphics system so it inherits
// the entire guest-side pipeline (SetupPresentation/SetupGuestGpu, the
// register file, the vsync worker, the ui::vulkan provider that stands up the
// instance/device/swapchain/VMA, and the base CommandProcessor). It only
// rebinds two seams:
//
//   - name()                    -> "fable2-vulkan"
//   - CreateCommandProcessor()  -> Fable2VulkanCommandProcessor (our hot path)
//
// Everything else is delegated. As the renderer is rewritten (see
// plans/custom-vulkan-renderer.md), Fable2VulkanCommandProcessor and its
// caches grow from thin overrides into the full custom implementation while
// this class stays a stable seam.

#include <memory>
#include <string>

#include <rex/graphics/vulkan/graphics_system.h>

namespace fable2::renderer {

class Fable2VulkanCommandProcessor;

class Fable2VulkanGraphicsSystem : public rex::graphics::vulkan::VulkanGraphicsSystem {
 public:
  Fable2VulkanGraphicsSystem() = default;

  std::string name() const override { return "fable2-vulkan"; }

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace fable2::renderer
