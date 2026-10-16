#pragma once
//
// fable2 :: renderer :: vulkan :: FramePresenter
//
// Workstream W2 — frame sync & presentation.
//
// A self-contained swapchain + N-in-flight-frame synchronizer built on top of
// VkContext (W1). It owns:
//   * a swapchain sized to the window (format/mode/extent picked from the
//     surface capabilities), one image view + framebuffer per image, and a
//     single-color-attachment render pass (initialLayout=UNDEFINED,
//     finalLayout=PRESENT_SRC) the caller renders into;
//   * a ring of `framesInFlight` slots, each with a fence (signaled on
//     submission), an acquire semaphore (signaled by vkAcquireNextImageKHR),
//     a render-complete semaphore (signaled on submission, waited on present),
//     and a one-time command pool + command buffer.
//
// The per-frame contract is a small three-step loop:
//
//     if (presenter->BeginFrame()) {        // wait slot fence (timeout ->
//        ...                                  //   drop frame), acquire the
//        // record into presenter->Cmd(),     //   image (timeout -> skip
//        //     using presenter->RenderPass() //   present)
//        //     + Framebuffer() + Extent()
//        ...
//        presenter->EndFrame();             // end cmd buffer, submit (fence),
//     }                                      // present (waits render sem)
//
// "Skip present rather than block" semantics: both the slot-fence wait and
// vkAcquireNextImageKHR use finite timeouts. If the GPU is behind (fence not
// signaled in time) the frame is dropped; if no swapchain image is ready in
// time the present is skipped but the slot still advances so the ring keeps
// moving. This decouples guest frame production from the host present when
// uncapped.
//
// Resize / robustness: vkAcquireNextImageKHR and vkQueuePresentKHR can return
// VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR (and surface-lost). Those set
// NeedsRecreate(); the caller then calls Recreate(newWidth, newHeight) which
// retires + rebuilds the swapchain.
//
// This class is deliberately independent of the ReXGlue SDK's ui::vulkan
// presenter so the custom renderer owns its own present path. The raw
// swapchain images / views are exposed so W3+ can build its own render passes
// over them, and so the UI/ImGui overlay pass (plan W2 "overlay compositing")
// can be drawn into the same frame after the game render.
//
#include <functional>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

struct FramePresenterConfig {
  // Number of in-flight frame slots in the sync ring (default 3, matching the
  // SDK's kSubmissionCount). Clamped down to the surface's max image count.
  uint32_t framesInFlight = 3;

  // Presentation mode selection (highest latency first as the fallback chain).
  // vsync=true  -> FIFO (or FIFO_RELAXED if allowed) : capped to display.
  // vsync=false -> IMMEDIATE if allowed (tearing/VRR), else MAILBOX (drop,
  //                low latency), else FIFO_RELAXED, else FIFO.
  bool vsync = true;
  bool allowFifoRelaxed = false;  // vsync=true: opt in to FIFO_RELAXED (cap but
                                  //   tear on long frames); default is strict FIFO
  bool allowMailbox = true;       // vsync=false: drop frames, no tearing
  bool allowImmediate = false;    // vsync=false: allow tearing / VRR

  // Finite timeouts (nanoseconds). On timeout the frame is dropped / the
  // present skipped instead of blocking (see class comment).
  uint64_t fenceTimeoutNs = 16'000'000;    // wait for a slot's GPU work
  uint64_t acquireTimeoutNs = 16'000'000;  // wait for a swapchain image

  // Load op for the swapchain image at the start of a frame. CLEAR gives the
  // classic "clear then draw" behavior; DONT_CARE is cheaper when the frame
  // fully covers the image.
  bool renderLoadClear = true;

  // Optional vblank pacing / frame-rate cap. When > 0, BeginFrame() sleeps to
  // maintain this frame rate (1e9 / targetFps ns per frame), so the host present
  // cadence can be matched to the guest's vblank (e.g. 60 Hz on a 144 Hz
  // display) even when the swapchain's present mode alone can't express it.
  // 0.0 = no pacing (present as fast as the present mode allows). This is the
  // "decouple guest frame production from the host present when uncapped" knob:
  // set it to the guest cadence, or leave it 0 to run uncapped.
  double targetFps = 0.0;
};

struct FramePresenterStats {
  uint64_t presented = 0;        // frames successfully presented
  uint64_t droppedFence = 0;     // slot fence not ready in time -> frame dropped
  uint64_t skippedAcquire = 0;   // no swapchain image ready in time -> no present
  uint64_t outOfDate = 0;        // VK_ERROR_OUT_OF_DATE_KHR / surface-lost events
  uint64_t suboptimal = 0;       // VK_SUBOPTIMAL_KHR events
  uint64_t frames = 0;           // total BeginFrame/EndFrame pairs completed
  double fps = 0.0;              // presented frames / elapsed seconds
};

enum class PresentStatus {
  kOk,           // presented (or skipped by design, e.g. acquire timeout)
  kSkipped,      // frame skipped (fence/acquire timeout, out-of-date)
  kSuboptimal,   // presented but swapchain is now suboptimal (recreate soon)
  kOutOfDate,    // swapchain/surface is out of date (recreate now)
  kDeviceLost,   // device lost (fatal for the session)
};

class FramePresenter {
 public:
  ~FramePresenter();
  FramePresenter(const FramePresenter&) = delete;
  FramePresenter& operator=(const FramePresenter&) = delete;

  // Creates the presenter for `surface` at `width`x`height`. Returns false and
  // sets `err` if the surface is unsupported, no suitable format exists, or a
  // Vulkan call fails.
  static bool Create(VkContext& ctx, VkSurfaceKHR surface, uint32_t width, uint32_t height,
                     const FramePresenterConfig& config, std::unique_ptr<FramePresenter>& out,
                     std::string& err);

  // ---- Per-frame loop (see class comment). ----
  // Waits for this slot's fence (finite timeout) and acquires a swapchain image
  // (finite timeout). Returns true if the frame should proceed (record +
  // EndFrame). Returns false if the frame was dropped (slot fence not ready);
  // in that case do not call EndFrame. If it returns true, check Skipped() to
  // decide whether to actually record (false means the present will be
  // skipped, e.g. acquire timed out).
  bool BeginFrame();
  // The command buffer to record into (valid after a successful BeginFrame).
  VkCommandBuffer Cmd() const { return slots_[slot_].cmd; }
  // The acquired swapchain image index (valid after a successful BeginFrame).
  uint32_t ImageIndex() const { return imageIndex_; }
  // The render pass / framebuffer / extent to render into for this frame.
  VkRenderPass RenderPass() const { return renderPass_; }
  VkFramebuffer Framebuffer() const { return framebuffers_[imageIndex_]; }
  VkExtent2D Extent() const { return extent_; }
  // True if the present will be skipped for this frame (acquire timeout /
  // out-of-date). The caller may skip recording to avoid wasted GPU work.
  bool Skipped() const { return skipPresent_; }

  // Ends the command buffer, submits it (signaling the slot fence + render
  // semaphore), and presents (waiting on the render semaphore). Returns the
  // present status. Must be called after a successful BeginFrame.
  PresentStatus EndFrame();

  // Convenience: clear the whole swapchain image to (r,g,b,a) and present.
  // Returns the present status, or kSkipped if the frame was dropped.
  PresentStatus PresentClear(float r, float g, float b, float a);

  // ---- Resize / robustness. ----
  // True if the last present/acquire reported the swapchain as suboptimal or
  // out of date; call Recreate() when the window size changes (or to recover).
  bool NeedsRecreate() const { return recreateNeeded_ || suboptimalPending_; }
  // Retires the current swapchain (awaiting in-flight work) and rebuilds it at
  // `width`x`height` using the same config. Returns false + sets `err` on
  // failure (e.g. zero size).
  bool Recreate(uint32_t width, uint32_t height, std::string& err);
  // Consumes a suboptimal/out-of-date report (after the caller has handled it).
  void ClearStatus();

  // Wait for all in-flight GPU work (shutdown / resize).
  void WaitIdle();

  // ---- Diagnostics / access for W3+. ----
  FramePresenterStats Stats() const;
  double ElapsedSeconds() const;
  VkPresentModeKHR PresentMode() const { return presentMode_; }
  VkFormat Format() const { return format_; }
  VkExtent2D Extent2D() const { return extent_; }
  uint32_t ImageCount() const { return uint32_t(images_.size()); }
  VkImage ImageAt(uint32_t i) const { return images_[i]; }
  VkImageView ImageViewAt(uint32_t i) const { return imageViews_[i]; }
  VkFramebuffer FramebufferAt(uint32_t i) const { return framebuffers_[i]; }
  const FramePresenterConfig& config() const { return config_; }

 private:
  FramePresenter() = default;
  bool Init(VkContext& ctx, VkSurfaceKHR surface, uint32_t width, uint32_t height,
            const FramePresenterConfig& config);
  // Picks format/extent/present mode and (re)creates the swapchain, image
  // views, framebuffers and render pass. Returns false + sets `err` on failure.
  bool BuildSwapchain(uint32_t width, uint32_t height, std::string& err);
  void DestroySwapchain();
  void CreateSlots();
  void DestroySlots();
  VkPresentModeKHR SelectPresentMode() const;
  void PickSurfaceFormat(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height,
                         VkSurfaceFormatKHR* outFormat, VkExtent2D* outExtent) const;

  VkContext* ctx_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  FramePresenterConfig config_{};

  // Swapchain state.
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> images_;
  std::vector<VkImageView> imageViews_;
  std::vector<VkFramebuffer> framebuffers_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent_ = {0, 0};
  uint32_t width_ = 0, height_ = 0;

  // Frame-sync ring.
  struct Slot {
    VkFence fence = VK_NULL_HANDLE;         // created signaled
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderSem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
  };
  std::vector<Slot> slots_;
  uint32_t slotCount_ = 0;
  uint32_t slot_ = 0;         // current slot (frameCounter % slotCount_)
  uint64_t frameCounter_ = 0;
  uint32_t imageIndex_ = 0;

  FramePresenterStats stats_{};
  bool skipPresent_ = false;
  bool recreateNeeded_ = false;  // out-of-date -> must recreate
  bool suboptimalPending_ = false;  // suboptimal -> recreate when convenient
  uint64_t startClockNs_ = 0;
  bool haveStartClock_ = false;
  // Vblank pacing state (used only when config_.targetFps > 0).
  uint64_t lastFrameNs_ = 0;
  bool haveLastFrameNs_ = false;
};

}  // namespace fable2::renderer::vulkan
