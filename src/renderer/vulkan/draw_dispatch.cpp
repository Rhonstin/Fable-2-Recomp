// W8 — Primitives, vertex/index buffers & draw dispatch implementation.
#include "draw_dispatch.h"

#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

VkPrimitiveTopology DrawDispatcher::MapTopology(XenosTopology topo) {
  switch (topo) {
    case XenosTopology::kPointList:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case XenosTopology::kLineList:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case XenosTopology::kLineStrip:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case XenosTopology::kTriangleList:
    case XenosTopology::kTriangleFan:      // SDK pre-translates to list
    case XenosTopology::kRectangleList:    // SDK pre-translates to list
    case XenosTopology::kQuadList:         // SDK pre-translates to list
    case XenosTopology::kQuadStrip:        // SDK pre-translates to list
    case XenosTopology::kPolygon:          // SDK pre-translates to list
    case XenosTopology::kTriangleWithWFlags:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case XenosTopology::kTriangleStrip:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case XenosTopology::kLineLoop:         // SDK pre-translates to line strip
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    default:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkViewport DrawDispatcher::MakeViewport(const ViewportState& vp) {
  VkViewport vkvp{};
  vkvp.x = vp.x;
  vkvp.y = vp.y;
  vkvp.width = vp.width;
  vkvp.height = vp.height;
  vkvp.minDepth = vp.minDepth;
  vkvp.maxDepth = vp.maxDepth;
  return vkvp;
}

VkRect2D DrawDispatcher::MakeScissor(const ScissorState& sc) {
  VkRect2D rect{};
  rect.offset = {static_cast<int32_t>(sc.x), static_cast<int32_t>(sc.y)};
  rect.extent = {sc.width, sc.height};
  return rect;
}

void DrawDispatcher::SetViewportScissor(VkCommandBuffer cmd, const ViewportState& vp,
                                        const ScissorState& sc, bool scissorEnabled) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  const VkViewport vkvp = MakeViewport(vp);
  f.vkCmdSetViewport(cmd, 0, 1, &vkvp);
  if (scissorEnabled) {
    const VkRect2D rect = MakeScissor(sc);
    f.vkCmdSetScissor(cmd, 0, 1, &rect);
  }
  viewportSets++;
}

void DrawDispatcher::BindVertexBuffer(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  const uint32_t binding = 0;
  f.vkCmdBindVertexBuffers(cmd, binding, 1, &buffer, &offset);
}

void DrawDispatcher::BindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, uint64_t offset,
                                     VkIndexType indexType) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  f.vkCmdBindIndexBuffer(cmd, buffer, offset, indexType);
}

void DrawDispatcher::Draw(VkCommandBuffer cmd, const DrawParams& params) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  // Set viewport + scissor if provided.
  if (params.viewport.width > 0.0f && params.viewport.height > 0.0f) {
    SetViewportScissor(cmd, params.viewport, params.scissor, params.scissorEnabled);
  }
  const float blend[4] = {params.blendR, params.blendG, params.blendB, params.blendA};
  f.vkCmdSetBlendConstants(cmd, blend);
  f.vkCmdDraw(cmd, params.vertexCount, params.instanceCount, params.firstVertex, 0);
  drawCalls++;
}

void DrawDispatcher::DrawIndexed(VkCommandBuffer cmd, const DrawParams& params,
                                 VkIndexType indexType) {
  if (!ctx_) {
    return;
  }
  Functions& f = ctx_->functions();
  // Set viewport + scissor if provided.
  if (params.viewport.width > 0.0f && params.viewport.height > 0.0f) {
    SetViewportScissor(cmd, params.viewport, params.scissor, params.scissorEnabled);
  }
  const float blend[4] = {params.blendR, params.blendG, params.blendB, params.blendA};
  f.vkCmdSetBlendConstants(cmd, blend);
  f.vkCmdDrawIndexed(cmd, params.indexCount, params.instanceCount, params.firstIndex,
                     params.baseVertex, 0);
  drawIndexedCalls++;
}

}  // namespace fable2::renderer::vulkan
