// W10 — Multi-threaded command encoding implementation.
#include "multi_thread_encoder.h"

#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

// --- WorkerEncoder ---

WorkerEncoder::~WorkerEncoder() { Shutdown(); }

bool WorkerEncoder::Init(VkContext* ctx, uint32_t maxCommandBufferSize) {
  ctx_ = ctx;
  Functions& f = ctx->functions();

  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = ctx->caps().graphicsFamily;
  if (f.vkCreateCommandPool(ctx->device(), &pci, nullptr, &pool_) != VK_SUCCESS) {
    return false;
  }

  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (f.vkAllocateCommandBuffers(ctx->device(), &ai, &cmdBuf_) != VK_SUCCESS) {
    f.vkDestroyCommandPool(ctx->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void WorkerEncoder::Shutdown() {
  if (ctx_ && cmdBuf_) {
    Functions& f = ctx_->functions();
    f.vkFreeCommandBuffers(ctx_->device(), pool_, 1, &cmdBuf_);
    cmdBuf_ = VK_NULL_HANDLE;
  }
  if (ctx_ && pool_) {
    ctx_->functions().vkDestroyCommandPool(ctx_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
  }
  ctx_ = nullptr;
}

bool WorkerEncoder::Submit(VkContext* ctx) {
  if (!dirty_ || !cmdBuf_) return false;
  Functions& f = ctx->functions();

  // Begin the command buffer.
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (f.vkBeginCommandBuffer(cmdBuf_, &bi) != VK_SUCCESS) {
    return false;
  }
  if (f.vkEndCommandBuffer(cmdBuf_) != VK_SUCCESS) {
    return false;
  }

  // Submit.
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmdBuf_;
  si.waitSemaphoreCount = 0;
  si.signalSemaphoreCount = 0;
  si.pWaitDstStageMask = &stage;

  if (f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
    return false;
  }
  submissions++;
  totalCommands++;
  dirty_ = false;
  return true;
}

// --- MultiThreadEncoder ---

MultiThreadEncoder::~MultiThreadEncoder() { Shutdown(); }

bool MultiThreadEncoder::Init(VkContext* ctx, uint32_t workerCount) {
  ctx_ = ctx;
  workers_.reserve(workerCount);
  for (uint32_t i = 0; i < workerCount; ++i) {
    auto w = std::make_unique<WorkerEncoder>();
    if (!w->Init(ctx)) {
      return false;
    }
    workers_.push_back(std::move(w));
  }
  return true;
}

void MultiThreadEncoder::Shutdown() {
  for (auto& w : workers_) {
    w->Shutdown();
  }
  workers_.clear();
  ctx_ = nullptr;
}

bool MultiThreadEncoder::Submit(VkContext* ctx) {
  bool anySubmitted = false;
  for (auto& w : workers_) {
    if (w->IsDirty() && w->Submit(ctx)) {
      anySubmitted = true;
      totalSubmitted++;
    }
  }
  if (anySubmitted) {
    frames++;
  }
  return anySubmitted;
}

}  // namespace fable2::renderer::vulkan
