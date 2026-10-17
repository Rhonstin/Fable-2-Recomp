#include "fable_2_init.h"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#include "src/core/xex_verify.h"
#include "src/diagnostics/fable2_frame_capture.h"

class Fable2ArmadaApp final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  void OnPostSetup() override {
    const char* output = std::getenv("FABLE2_CAPTURE_DIR");
    if (!output || !*output) return;
    const std::filesystem::path directory(output);
    std::filesystem::create_directories(directory);
    auto* presenter = window()->graphics_presenter();
    capture_thread_ = std::jthread([directory, presenter](std::stop_token stop) {
      unsigned sequence = 0;
      while (!stop.stop_requested()) {
        for (int tick = 0; tick < 50 && !stop.stop_requested(); ++tick)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop.stop_requested()) break;
        const auto path = directory / ("frame-" + std::to_string(sequence++) + ".png");
        const auto result = fable2::framecapture::CaptureGuestOutputToPng(presenter, path.string());
        std::fprintf(stderr, "FRAME_CAPTURE ok=%d size=%dx%d luma=%d path=%s error=%s\n",
                     result.ok, result.width, result.height, result.avg_luma,
                     path.c_str(), result.error.c_str());
      }
    });
  }

  void OnShutdown() override {
    capture_thread_.request_stop();
    if (capture_thread_.joinable()) capture_thread_.join();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& context) {
    return std::unique_ptr<Fable2ArmadaApp>(new Fable2ArmadaApp(context, "fable_2", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";
    config.graphics = rex::system::LoadGpuPlugin("xenos", "vulkan");
    if (!config.graphics) throw std::runtime_error("Cannot initialize the native Vulkan GPU plugin");
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME is not set");
    const auto xdg = [home](const char* variable, const char* fallback) {
      const char* value = std::getenv(variable);
      return value && *value ? std::filesystem::path(value) : std::filesystem::path(home) / fallback;
    };
    // Keep this executable revision's saves separate from GOTY installations.
    paths.user_data_root = xdg("XDG_DATA_HOME", ".local/share") / "fable2-native/retail-ru";
    paths.cache_root = xdg("XDG_CACHE_HOME", ".cache") / "fable2-native/retail-ru";
    const auto configuration = xdg("XDG_CONFIG_HOME", ".config") / "fable2-native/retail-ru";
    for (const auto& directory : {paths.user_data_root, paths.cache_root, configuration})
      std::filesystem::create_directories(directory);
    paths.config_path = configuration / "fable_2.toml";
  }

  void OnLoadXexImage(std::string&) override {
    std::string hash;
    if (!fable2::xexverify::Sha256File(game_data_root() / "default.xex", hash) ||
        hash != FABLE2_XEX_SHA256)
      throw std::runtime_error("default.xex differs from the executable used for this native build");
    if (!std::filesystem::is_regular_file(game_data_root() / "data/dir.manifest"))
      throw std::runtime_error("Game content is missing: data/dir.manifest");
  }

 private:
  std::jthread capture_thread_;
};

REX_DEFINE_APP(fable_2, Fable2ArmadaApp::Create)
