// rexgpu-fable2vulkan: custom high-performance Vulkan GPU plugin for the
// Fable 2 ReXGlue recompile.
//
// This is a from-scratch Vulkan renderer that implements the ReXGlue GPU
// plugin ABI (see include/rex/system/gpu_plugin.h). It reuses the SDK's
// backend-agnostic guest-side graphics pipeline (register file, packet
// disassembler, command-processor base, Xenos register-bank -> SPIR-V shader
// translator, format/ucode tables, shared memory, vsync worker) and the SDK's
// ui::vulkan provider (instance/device/swapchain/VMA/UI), and builds a
// performance-focused Vulkan host side on top of them.
//
// The plugin is loaded at launch via --gpu_plugin=fable2-vulkan (see
// src/core/fable_2_app.h). It is deliberately a separate shared library so it
// can be A/B'd against the stock rexgpu-xenos (D3D12) plugin with no rebuild.
//
// See plans/custom-vulkan-renderer.md for the full design and checklist.

#include <string_view>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "fable2_vulkan_graphics_system.h"

namespace {

constexpr char kName[] = "rexgpu-fable2vulkan";

}  // namespace

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("{}: host requested ABI {}, plugin is ABI {}", kName, abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("{}: invalid GpuCreateInfo", kName);
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
  // This plugin is Vulkan-only; accept "vulkan" (or "any").
  if (backend == "any" || backend == "vulkan") {
    return new fable2::renderer::Fable2VulkanGraphicsSystem();
  }

  REXLOG_ERROR("{}: requested backend '{}' is not supported (this plugin is Vulkan-only)",
               kName, backend);
  return nullptr;
}
