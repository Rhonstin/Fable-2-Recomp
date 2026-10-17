// fable2 :: renderer :: vulkan :: smoke-test entry point
//
// A tiny console harness for the VkContext foundation self-test (W1):
//   fable2-vk-smoke [--validation] [--device NAME] [--report FILE]
//                   [--cache FILE] [--budget-mb N]
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "vk_pipeline_smoke.h"
#include "vk_present_smoke.h"
#include "vk_renderpass_smoke.h"
#include "vk_draw_smoke.h"
#include "vk_shader_smoke.h"
#include "vk_perf_smoke.h"
#include "vk_robustness_smoke.h"
#include "vk_rt_smoke.h"
#include "vk_shared_blit_smoke.h"
#include "vk_smoke.h"
#include "vk_texture_smoke.h"

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so a crash loses no output
  fable2::renderer::vulkan::VkContextConfig cfg;
  fable2::renderer::vulkan::PresentSmokeConfig psmoke;
  std::string report;
  bool present = false;
  bool renderpass = false;
  bool pipeline = false;
  bool texture = false;
  bool rt = false;
  bool shared = false;
  bool draw = false;
  bool shader = false;
  bool perf = false;
  bool robustness = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--validation") {
      cfg.withValidation = true;
    } else if (a == "--device" && i + 1 < argc) {
      cfg.forcedDeviceName = argv[++i];
    } else if (a == "--report" && i + 1 < argc) {
      report = argv[++i];
    } else if (a == "--cache" && i + 1 < argc) {
      cfg.pipelineCachePath = argv[++i];
    } else if (a == "--budget-mb" && i + 1 < argc) {
      cfg.memoryBudgetBytes = static_cast<uint64_t>(std::stoul(argv[++i])) * 1024 * 1024;
    } else if (a == "--renderpass") {
      renderpass = true;
    } else if (a == "--pipeline") {
      pipeline = true;
    } else if (a == "--texture") {
      texture = true;
    } else if (a == "--rt") {
      rt = true;
    } else if (a == "--shared") {
      shared = true;
    } else if (a == "--draw") {
      draw = true;
    } else if (a == "--shader") {
      shader = true;
    } else if (a == "--perf") {
      perf = true;
    } else if (a == "--robustness") {
      robustness = true;
    } else if (a == "--present") {
      present = true;
    } else if (a == "--present-seconds" && i + 1 < argc) {
      present = true;
      psmoke.seconds = std::stod(argv[++i]);
    } else if (a == "--no-vsync") {
      psmoke.vsync = false;
    } else if (a == "--tear") {
      psmoke.allowImmediate = true;
    } else if (a == "--resize") {
      psmoke.doResize = true;
    } else if (a == "--pw" && i + 1 < argc) {
      psmoke.width = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--ph" && i + 1 < argc) {
      psmoke.height = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--cap" && i + 1 < argc) {
      psmoke.capFps = std::stod(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      std::cout << "fable2-vk-smoke [--validation] [--device NAME] [--report FILE]\n"
                   "                [--cache FILE] [--budget-mb N]\n"
                   "                [--renderpass] [--pipeline] [--texture] [--rt]\n"
                   "                [--shared] [--draw]\n"
                   "                [--present] [--present-seconds N] [--no-vsync]\n"
                   "                [--tear] [--resize] [--cap N] [--pw W] [--ph H]\n";
      return 0;
    }
  }
  if (pipeline) {
    return fable2::renderer::vulkan::RunPipelineSmokeTest(cfg, report);
  }
  if (texture) {
    return fable2::renderer::vulkan::RunTextureSmokeTest(cfg, report);
  }
  if (rt) {
    return fable2::renderer::vulkan::RunRenderTargetSmokeTest(cfg, report);
  }
  if (shared) {
    return fable2::renderer::vulkan::RunSharedBlitSmokeTest(cfg, report);
  }
  if (draw) {
    return fable2::renderer::vulkan::RunDrawSmokeTest(cfg, report);
  }
  if (shader) {
    return fable2::renderer::vulkan::RunShaderSmokeTest(cfg, report);
  }
  if (perf) {
    return fable2::renderer::vulkan::RunPerfSmokeTest() ? 0 : 1;
  }
  if (robustness) {
    return fable2::renderer::vulkan::RunRobustnessSmokeTest() ? 0 : 1;
  }
  if (renderpass) {
    return fable2::renderer::vulkan::RunRenderPassSmokeTest(cfg, report);
  }
  if (present) {
    return fable2::renderer::vulkan::RunPresentSmokeTest(cfg, psmoke, report);
  }
  return fable2::renderer::vulkan::RunSmokeTest(cfg, report);
}
