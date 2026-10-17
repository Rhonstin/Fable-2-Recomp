// W10 — Multi-threaded command encoding.
//
// Records guest RT passes on worker threads in parallel with the main thread.
// The main thread submits the workers' command buffers. This is safe because
// Vulkan command buffers are thread-safe to record (as long as each buffer
// is recorded by only one thread) and the submission is serialized on the
// main thread.
//
// The key insight: if the guest frame maps to independent render targets
// (e.g., scene → shadow map → post-FX), each can be recorded by a different
// worker thread. The main thread then submits them in order.

#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// A single worker encoder: owns a command pool + buffer, records commands
// on its thread, and the main thread submits the result.
class WorkerEncoder {
 public:
  WorkerEncoder() = default;
  ~WorkerEncoder();

  bool Init(VkContext* ctx, uint32_t maxCommandBufferSize = 1024 * 1024);
  void Shutdown();

  // Begin a new frame: reset the command buffer for recording.
  void BeginFrame() {
    frame_++;
    dirty_ = false;
  }

  // Record commands on this worker's command buffer.
  // The callback receives the command buffer to record into.
  void Record(std::function<void(VkCommandBuffer)> fn) {
    if (!cmdBuf_) return;
    fn(cmdBuf_);
    dirty_ = true;
  }

  // Submit this worker's command buffer to the graphics queue.
  // Must be called from the main thread.
  bool Submit(VkContext* ctx);

  // Accessors.
  VkCommandBuffer commandBuffer() const { return cmdBuf_; }
  uint32_t frame() const { return frame_; }
  bool IsDirty() const { return dirty_; }
  uint64_t submissions = 0;
  uint64_t totalCommands = 0;  // approximate (callback call count).

 private:
  VkContext* ctx_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmdBuf_ = VK_NULL_HANDLE;
  uint32_t frame_ = 0;
  bool dirty_ = false;
};

// Multi-threaded encoder manager: a pool of WorkerEncoders + a thread pool.
// The main thread assigns work to workers and collects results.
class MultiThreadEncoder {
 public:
  MultiThreadEncoder() = default;
  ~MultiThreadEncoder();

  bool Init(VkContext* ctx, uint32_t workerCount = 2);
  void Shutdown();

  // Begin a new frame: reset all workers.
  void BeginFrame() {
    for (auto& w : workers_) w->BeginFrame();
  }

  // Assign work to a worker (record commands on that worker's buffer).
  void Assign(uint32_t workerIndex, std::function<void(VkCommandBuffer)> fn) {
    if (workerIndex < workers_.size()) {
      workers_[workerIndex]->Record(std::move(fn));
      totalAssigned++;
    }
  }

  // Submit all dirty workers' command buffers (in order).
  // Must be called from the main thread.
  bool Submit(VkContext* ctx);

  // Accessors.
  uint32_t WorkerCount() const { return (uint32_t)workers_.size(); }
  uint64_t frames = 0;
  uint64_t totalAssigned = 0;
  uint64_t totalSubmitted = 0;

 private:
  VkContext* ctx_ = nullptr;
  std::vector<std::unique_ptr<WorkerEncoder>> workers_;
};

}  // namespace fable2::renderer::vulkan
