// W8 — Primitives, vertex/index buffers & draw dispatch.
//
// Maps Xenos primitive topology to Vulkan, sets up viewport/scissor from
// guest rasterizer state, and records draw calls (vkCmdDraw /
// vkCmdDrawIndexed). Handles 16-bit and 32-bit index types, and supports
// multi-viewport draws (Xenos has up to 5 viewports).
//
// The full primitive processor (triangle fan → list conversion, quad →
// triangle expansion, adaptive tessellation) is handled by the SDK's
// pre-translated shaders (W9); this module provides the Vulkan-side
// dispatch that consumes the normalized primitive data.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vk_context.h"

namespace fable2::renderer::vulkan {

// Xenos primitive topology → Vulkan topology mapping.
enum class XenosTopology : uint32_t {
  kNone = 0x00,
  kPointList = 0x01,
  kLineList = 0x02,
  kLineStrip = 0x03,
  kTriangleList = 0x04,
  kTriangleFan = 0x05,   // SDK pre-translates to triangle list
  kTriangleStrip = 0x06,
  kTriangleWithWFlags = 0x07,
  kRectangleList = 0x08,  // SDK pre-translates to triangle list
  kLineLoop = 0x0C,       // SDK pre-translates to line strip
  kQuadList = 0x0D,       // SDK pre-translates to triangle list
  kQuadStrip = 0x0E,      // SDK pre-translates to triangle list
  kPolygon = 0x0F,        // SDK pre-translates to triangle list
};

// Rasterizer mode (from the guest rasterizer state).
enum class RasterizerMode : uint32_t {
  kPolygon = 0,  // standard fill
  kPoint = 1,    // point sprite
  kLine = 2,     // line rasterization
};

// Viewport state from the guest rasterizer.
struct ViewportState {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float minDepth = 0.0f;
  float maxDepth = 1.0f;
};

// Scissor state from the guest rasterizer.
struct ScissorState {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Draw parameters for a single draw call.
struct DrawParams {
  XenosTopology topology = XenosTopology::kTriangleList;
  RasterizerMode mode = RasterizerMode::kPolygon;
  bool indexed = false;
  bool index32 = false;  // 32-bit vs 16-bit indices
  uint32_t vertexCount = 0;
  uint32_t instanceCount = 1;
  uint32_t firstVertex = 0;
  uint32_t indexCount = 0;
  uint32_t firstIndex = 0;
  int32_t baseVertex = 0;
  // Viewport + scissor.
  ViewportState viewport;
  ScissorState scissor;
  bool scissorEnabled = false;
  // Blend constants (from the guest blend state).
  float blendR = 1.0f;
  float blendG = 1.0f;
  float blendB = 1.0f;
  float blendA = 1.0f;
};

class DrawDispatcher {
 public:
  explicit DrawDispatcher(VkContext* ctx) : ctx_(ctx) {}

  // Map a Xenos topology to a Vulkan primitive topology.
  static VkPrimitiveTopology MapTopology(XenosTopology topo);

  // Set the viewport + scissor dynamic state in a command buffer.
  void SetViewportScissor(VkCommandBuffer cmd, const ViewportState& vp,
                          const ScissorState& sc, bool scissorEnabled);

  // Record a non-indexed draw call.
  void Draw(VkCommandBuffer cmd, const DrawParams& params);

  // Record an indexed draw call.
  void DrawIndexed(VkCommandBuffer cmd, const DrawParams& params,
                   VkIndexType indexType);

  // Record an indirect non-indexed draw (GPU-instanced). The draw parameters
  // (vertexCount, instanceCount, firstVertex, firstInstance) are read from
  // a VkBuffer on the GPU.
  void DrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset,
                    uint32_t drawCount = 1, uint32_t stride = 0);

  // Record an indirect indexed draw (GPU-instanced).
  void DrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset,
                           uint32_t drawCount = 1, uint32_t stride = 0);

  // Set multiple viewports + scissors (up to 5 on Xenos).
  void SetMultiViewportScissor(VkCommandBuffer cmd, const ViewportState* viewports,
                               const ScissorState* scissors, uint32_t count,
                               bool scissorEnabled);

  // Bind the vertex buffer (from shared memory or an uploaded buffer).
  void BindVertexBuffer(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset);

  // Bind the index buffer.
  void BindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset,
                       VkIndexType indexType);

  // Build the VkViewport from a ViewportState.
  static VkViewport MakeViewport(const ViewportState& vp);
  // Build the VkRect2D from a ScissorState.
  static VkRect2D MakeScissor(const ScissorState& sc);

  uint64_t drawCalls = 0;
  uint64_t drawIndexedCalls = 0;
  uint64_t drawIndirectCalls = 0;
  uint64_t drawIndexedIndirectCalls = 0;
  uint64_t viewportSets = 0;
  uint64_t multiViewportSets = 0;

 private:
  VkContext* ctx_;
};

}  // namespace fable2::renderer::vulkan
