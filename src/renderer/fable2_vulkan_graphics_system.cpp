// See fable2_vulkan_graphics_system.h.

#include "fable2_vulkan_graphics_system.h"

#include "fable2_vulkan_command_processor.h"

namespace fable2::renderer {

std::unique_ptr<rex::graphics::CommandProcessor>
Fable2VulkanGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<Fable2VulkanCommandProcessor>(this, kernel_state_);
}

}  // namespace fable2::renderer
