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

#include "vk_smoke.h"

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered so a crash loses no output
  fable2::renderer::vulkan::VkContextConfig cfg;
  std::string report;
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
    } else if (a == "--help" || a == "-h") {
      std::cout << "fable2-vk-smoke [--validation] [--device NAME] [--report FILE]\n"
                   "                [--cache FILE] [--budget-mb N]\n";
      return 0;
    }
  }
  return fable2::renderer::vulkan::RunSmokeTest(cfg, report);
}
