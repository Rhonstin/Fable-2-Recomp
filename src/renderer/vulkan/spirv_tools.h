#pragma once
//
// fable2 :: renderer :: vulkan :: SpirvToolsContext
//
// A minimal SPIRV-Tools context loaded at runtime (like the ReXGlue SDK does),
// so the plugin carries no SPIRV-Tools import-library dependency. Used by the
// shader pipeline (W7) to validate/optimise translated SPIR-V; owned by the
// VkContext so the whole toolchain lives in one place.
//
#include <cstdint>
#include <string>

#include <spirv-tools/libspirv.h>

#include "vk_dynlib.h"

namespace fable2::renderer::vulkan {

class SpirvToolsContext {
 public:
  SpirvToolsContext() = default;
  ~SpirvToolsContext() { Shutdown(); }
  SpirvToolsContext(const SpirvToolsContext&) = delete;
  SpirvToolsContext& operator=(const SpirvToolsContext&) = delete;

  // `env` is the SPIR-V target environment the renderer translates to (e.g.
  // SPV_ENV_VULKAN_1_1_SPIRV_1_4). Returns true if a usable context was created.
  bool Initialize(spv_target_env env, std::string& err);
  void Shutdown();

  spv_context context() const { return context_; }
  bool IsValid() const { return context_ != nullptr; }

  // Validate a raw SPIR-V binary. Returns true if valid.
  // `err` receives a diagnostic message on failure.
  bool Validate(const uint32_t* words, size_t wordCount, std::string& err) {
    if (!IsValid() || !fn_spvValidateBinary_) {
      return true;  // Validation unavailable — non-fatal.
    }
    spv_diagnostic diagnostic = nullptr;
    spv_result_t result = fn_spvValidateBinary_(context_, words, wordCount, &diagnostic);
    if (result != SPV_SUCCESS) {
      if (diagnostic) {
        err = diagnostic->error ? diagnostic->error : "unknown error";
        fn_spvDiagnosticDestroy_(diagnostic);
      } else {
        err = "SPIR-V validation failed (no diagnostic)";
      }
      return false;
    }
    return true;
  }

 private:
  DynLib library_;
  spv_context (*fn_spvContextCreate_)(spv_target_env) = nullptr;
  void (*fn_spvContextDestroy_)(spv_context) = nullptr;
  spv_result_t (*fn_spvValidateBinary_)(spv_const_context, const uint32_t*, size_t, spv_diagnostic*) = nullptr;
  void (*fn_spvDiagnosticDestroy_)(spv_diagnostic) = nullptr;
  spv_context context_ = nullptr;
};

}  // namespace fable2::renderer::vulkan
