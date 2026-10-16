// fable2 :: renderer :: vulkan :: present smoke test (W2)
//
// Windowed self-test for the FramePresenter. See vk_present_smoke.h.
//
#include "vk_present_smoke.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "frame_presenter.h"

#include <vulkan/vulkan_win32.h>  // VkWin32SurfaceCreateInfoKHR + vkCreateWin32SurfaceKHR

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fable2::renderer::vulkan {
namespace {

#ifdef _WIN32
constexpr wchar_t kClassName[] = L"f2vk_present_smoke";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Creates a top-level Win32 window sized `width`x`height` (client area is
// approximately that; the smoke test reads the real client size for the
// swapchain). Returns the HWND, or nullptr on failure. (Named CreateSmokeWindow
// to avoid colliding with the Win32 CreateWindow macro.)
HWND CreateSmokeWindow(HINSTANCE hinst, const wchar_t* title, uint32_t width, uint32_t height) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc)) {
    return nullptr;
  }
  HWND hwnd =
      CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                      width, height, nullptr, nullptr, hinst, nullptr);
  if (!hwnd) {
    return nullptr;
  }
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  return hwnd;
}
#endif  // _WIN32

// HSV (h in [0,1), s, v in [0,1]) -> RGB [0,1].
void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
  h = h - std::floor(h);
  float hh = h * 6.0f;
  float i = std::floor(hh);
  float f = hh - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));
  switch (int(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

const char* PresentModeName(VkPresentModeKHR m) {
  switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "UNKNOWN";
  }
}

class Report {
 public:
  explicit Report(const std::string& file) {
    lines_.reserve(64);
    if (!file.empty()) {
      out_.open(file);
    }
  }
  ~Report() {
    if (out_.is_open()) {
      out_.close();
    }
  }
  void Line(const std::string& s) {
    lines_.push_back(s);
    std::printf("%s\n", s.c_str());
    if (out_.is_open()) {
      out_ << s << '\n';
    }
  }
 private:
  std::vector<std::string> lines_;
  std::ofstream out_;
};

#ifdef _WIN32
bool RunPresentLoop(const PresentSmokeConfig& cfg, VkContext& ctx, HWND hwnd, HINSTANCE hinst,
                    Report& rep) {
  // Resolve vkCreateWin32SurfaceKHR and create the surface.
  using CreateWin32SurfaceFn = PFN_vkCreateWin32SurfaceKHR;
  auto createSurface = reinterpret_cast<CreateWin32SurfaceFn>(
      ctx.functions().vkGetInstanceProcAddr(ctx.instance(), "vkCreateWin32SurfaceKHR"));
  if (!createSurface) {
    rep.Line("FAIL: vkCreateWin32SurfaceKHR not resolved");
    return false;
  }
  VkWin32SurfaceCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  sci.hinstance = hinst;
  sci.hwnd = hwnd;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (createSurface(ctx.instance(), &sci, nullptr, &surface) != VK_SUCCESS) {
    rep.Line("FAIL: vkCreateWin32SurfaceKHR failed");
    return false;
  }

  // Build the frame presenter.
  FramePresenterConfig fpCfg;
  fpCfg.vsync = cfg.vsync;
  fpCfg.allowImmediate = cfg.allowImmediate;
  fpCfg.allowMailbox = cfg.allowMailbox;
  fpCfg.targetFps = cfg.capFps;
  std::unique_ptr<FramePresenter> presenter;
  std::string err;
  if (!FramePresenter::Create(ctx, surface, cfg.width, cfg.height, fpCfg, presenter, err)) {
    rep.Line("FAIL: FramePresenter::Create: " + err);
    ctx.functions().vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
    return false;
  }

  rep.Line("swapchain: " + std::to_string(presenter->Extent2D().width) + "x" +
           std::to_string(presenter->Extent2D().height) +
           ", format=" + std::to_string(uint32_t(presenter->Format())) +
           ", presentMode=" + PresentModeName(presenter->PresentMode()) +
           ", images=" + std::to_string(presenter->ImageCount()) +
           ", framesInFlight=" + std::to_string(fpCfg.framesInFlight));

  // Present loop.
  const auto start = std::chrono::steady_clock::now();
  const double durationSec = cfg.seconds;
  uint64_t frames = 0;
  uint32_t lastW = presenter->Extent2D().width;
  uint32_t lastH = presenter->Extent2D().height;
  bool didResize = false;
  uint64_t presentedBeforeResize = 0;
  bool recreateFailed = false;

  const auto now = [] { return std::chrono::steady_clock::now(); };

  while (true) {
    double elapsed = std::chrono::duration<double>(now() - start).count();
    if (elapsed >= durationSec) {
      break;
    }

    // Pump window messages (non-blocking).
    MSG msg;
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        quit = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quit) {
      break;
    }

    // Mid-run resize to exercise swapchain recreate.
    if (cfg.doResize && !didResize && elapsed >= durationSec * 0.5) {
      didResize = true;
      presentedBeforeResize = presenter->Stats().presented;
      SetWindowPos(hwnd, nullptr, 0, 0, cfg.width + 320, cfg.height + 200,
                   SWP_NOZORDER | SWP_NOMOVE);
    }

    // Track the window's client size; recreate the swapchain when it changes or
    // when a present reported suboptimal/out-of-date.
    RECT rc;
    if (GetClientRect(hwnd, &rc)) {
      uint32_t w = uint32_t(rc.right - rc.left);
      uint32_t h = uint32_t(rc.bottom - rc.top);
      if ((w != lastW || h != lastH) || presenter->NeedsRecreate()) {
        if (w > 0 && h > 0) {
          std::string rerr;
          if (presenter->Recreate(w, h, rerr)) {
            lastW = presenter->Extent2D().width;
            lastH = presenter->Extent2D().height;
            presenter->ClearStatus();
          } else {
            recreateFailed = true;
            rep.Line("resize recreate failed: " + rerr);
          }
        }
      }
    }

    // Present a hue-cycling clear color.
    float hue = float(std::fmod(elapsed * 0.25, 1.0));  // one hue cycle every 4s
    float r, g, b;
    HsvToRgb(hue, 0.8f, 1.0f, r, g, b);
    presenter->PresentClear(r, g, b, 1.0f);
    frames++;
  }

  // Final drain so stats are accurate.
  presenter->WaitIdle();
  FramePresenterStats st = presenter->Stats();

  rep.Line("");
  rep.Line("run: vsync=" + std::string(cfg.vsync ? "on (FIFO)" : "off") +
           (cfg.capFps > 0.0 ? (", cap=" + std::to_string(int(cfg.capFps)) + "fps") : "") +
           ", elapsed=" + std::to_string(presenter->ElapsedSeconds()).substr(0, 5) + "s");
  rep.Line("frames submitted=" + std::to_string(frames) +
           ", presented=" + std::to_string(st.presented) +
           ", fps=" + std::to_string(st.fps).substr(0, 6));
  rep.Line("dropped(fence)=" + std::to_string(st.droppedFence) +
           ", skipped(acquire)=" + std::to_string(st.skippedAcquire) +
           ", outOfDate=" + std::to_string(st.outOfDate) +
           ", suboptimal=" + std::to_string(st.suboptimal));
  if (cfg.doResize) {
    uint64_t after = st.presented - presentedBeforeResize;
    rep.Line("resize: presented before=" + std::to_string(presentedBeforeResize) +
             ", after=" + std::to_string(after) +
             (after > 0 ? " (recreate OK)" : " (recreate FAILED)"));
    if (after == 0) {
      recreateFailed = true;
    }
  }

  bool ok = st.presented > 0 && !recreateFailed;
  rep.Line(ok ? "PRESENT SMOKE: PASS" : "PRESENT SMOKE: FAIL");

  // Cleanup: retire the swapchain (presenter) before the surface, then the
  // surface itself.
  presenter.reset();
  if (ctx.functions().vkDestroySurfaceKHR) {
    ctx.functions().vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
  }
  return ok;
}
#endif  // _WIN32

}  // namespace

int RunPresentSmokeTest(const VkContextConfig& ctxConfig, const PresentSmokeConfig& cfg,
                        const std::string& reportFile) {
#ifdef _WIN32
  Report rep(reportFile);
  rep.Line("=== fable2-vk present smoke (W2) ===");

  // Create the context (with presentation enabled).
  VkContextConfig cc = ctxConfig;
  cc.withPresentation = true;
  std::unique_ptr<VkContext> ctx;
  std::string cerr;
  if (!VkContext::Create(cc, ctx, cerr)) {
    rep.Line("FAIL: VkContext::Create: " + cerr);
    return 1;
  }
  rep.Line("device: " + ctx->caps().deviceName + " (Vulkan " +
           std::to_string(ctx->caps().deviceApiVersion / 100000) + "." +
           std::to_string((ctx->caps().deviceApiVersion / 1000) % 100) + ")");

  HINSTANCE hinst = GetModuleHandleW(nullptr);
  HWND hwnd = CreateSmokeWindow(hinst, L"fable2-vk present smoke (W2)", cfg.width, cfg.height);
  if (!hwnd) {
    rep.Line("FAIL: could not create the Win32 window");
    return 1;
  }

  bool ok = RunPresentLoop(cfg, *ctx, hwnd, hinst, rep);

  DestroyWindow(hwnd);
  UnregisterClassW(kClassName, hinst);
  ctx.reset();
  return ok ? 0 : 1;
#else
  (void)ctxConfig;
  (void)cfg;
  (void)reportFile;
  std::printf("PRESENT SMOKE: not supported on this platform (needs a Win32 window)\n");
  return 1;
#endif
}

}  // namespace fable2::renderer::vulkan
