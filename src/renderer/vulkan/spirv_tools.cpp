#include "spirv_tools.h"

#include <array>

namespace fable2::renderer::vulkan {

bool SpirvToolsContext::Initialize(spv_target_env env, std::string& err) {
  Shutdown();

  // Candidate library names (the Vulkan SDK ships SPIRV-Tools-shared; distros
  // ship libSPIRV-Tools-shared.so).
  static const char* kCandidates[] = {
      "SPIRV-Tools-shared.dll", "SPIRV-Tools.dll", "libSPIRV-Tools-shared.so",
      "libSPIRV-Tools.so", "libSPIRV-Tools.dylib",
  };
  bool loaded = false;
  for (const char* name : kCandidates) {
    if (library_.Load(name)) {
      loaded = true;
      break;
    }
  }
  if (!loaded) {
    err = "SPIRV-Tools shared library not found";
    return false;
  }

  fn_spvContextCreate_ =
      library_.GetSymbolAs<decltype(fn_spvContextCreate_)>("spvContextCreate");
  fn_spvContextDestroy_ =
      library_.GetSymbolAs<decltype(fn_spvContextDestroy_)>("spvContextDestroy");
  if (!fn_spvContextCreate_ || !fn_spvContextDestroy_) {
    err = "SPIRV-Tools: spvContextCreate/spvContextDestroy not found";
    Shutdown();
    return false;
  }

  context_ = fn_spvContextCreate_(env);
  if (!context_) {
    err = "SPIRV-Tools: spvContextCreate returned null";
    Shutdown();
    return false;
  }
  err.clear();
  return true;
}

void SpirvToolsContext::Shutdown() {
  if (context_ && fn_spvContextDestroy_) {
    fn_spvContextDestroy_(context_);
  }
  context_ = nullptr;
  library_.Close();
  fn_spvContextCreate_ = nullptr;
  fn_spvContextDestroy_ = nullptr;
}

}  // namespace fable2::renderer::vulkan
