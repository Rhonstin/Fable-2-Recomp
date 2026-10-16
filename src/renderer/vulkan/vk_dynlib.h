#pragma once
//
// fable2 :: renderer :: vulkan :: DynLib
//
// A tiny self-contained dynamic-library loader (Win32 LoadLibrary / POSIX
// dlopen). Used by the custom renderer to resolve the Vulkan loader and
// SPIRV-Tools at runtime without a link-time import library, and without
// depending on the SDK's rex::platform::DynamicLibrary (whose methods are not
// exported from rexruntime).
//
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fable2::renderer::vulkan {

class DynLib {
 public:
  DynLib() = default;
  ~DynLib() { Close(); }
  DynLib(const DynLib&) = delete;
  DynLib& operator=(const DynLib&) = delete;

  // Loads the library (by name or path). Returns true on success.
  bool Load(const std::string& name);
  void Close();

  void* GetSymbol(const char* name) const { return handle_ ? GetRawSymbol(name) : nullptr; }
  template <typename T>
  T GetSymbolAs(const char* name) const {
    return reinterpret_cast<T>(GetSymbol(name));
  }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  void* GetRawSymbol(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
  }
  void* handle_ = nullptr;
};

inline bool DynLib::Load(const std::string& name) {
  Close();
#ifdef _WIN32
  handle_ = reinterpret_cast<void*>(LoadLibraryA(name.c_str()));
#else
  handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif
  return handle_ != nullptr;
}

inline void DynLib::Close() {
  if (!handle_) return;
#ifdef _WIN32
  FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
}

}  // namespace fable2::renderer::vulkan
