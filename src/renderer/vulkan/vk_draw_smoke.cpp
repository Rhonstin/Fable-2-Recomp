// W8 — Primitives, vertex/index buffers & draw dispatch smoke test.
#include "vk_draw_smoke.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/Initialize.h>
#include <glslang/Public/ShaderLang.h>

#include "descriptor_manager.h"
#include "draw_dispatch.h"
#include "pipeline_manager.h"
#include "render_pass_manager.h"
#include "vk_function_table.h"

namespace fable2::renderer::vulkan {

namespace {

const TBuiltInResource& DefaultResource() {
  static const TBuiltInResource resource = [] {
    TBuiltInResource r{};
    r.maxLights = 32;
    r.maxClipPlanes = 6;
    r.maxTextureUnits = 32;
    r.maxTextureCoords = 32;
    r.maxVertexAttribs = 64;
    r.maxVertexUniformComponents = 4096;
    r.maxVaryingFloats = 64;
    r.maxVertexTextureImageUnits = 32;
    r.maxCombinedTextureImageUnits = 80;
    r.maxTextureImageUnits = 32;
    r.maxFragmentUniformComponents = 4096;
    r.maxDrawBuffers = 32;
    r.maxVertexOutputVectors = 16;
    r.maxFragmentInputVectors = 15;
    r.maxClipDistances = 8;
    r.maxViewports = 16;
    r.maxFragmentImageUniforms = 8;
    return r;
  }();
  return resource;
}

bool CompileGlslToSpirv(EShLanguage stage, const char* src, std::vector<uint32_t>& out,
                        std::string& err) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  glslang::TShader shader(stage);
  shader.setStrings(&src, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&DefaultResource(), 450, false, messages)) {
    err = "GLSL parse: ";
    if (const char* log = shader.getInfoLog()) err += log;
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    err = "GLSL link: ";
    if (const char* log = program.getInfoLog()) err += log;
    return false;
  }
  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (!intermediate) {
    err = "no stage intermediate";
    return false;
  }
  glslang::SpvOptions opts = {};
  opts.disableOptimizer = true;
  glslang::GlslangToSpv(*intermediate, out, &opts);
  return !out.empty();
}

void line(bool ok, const std::string& msg, std::string& report, bool& allOk) {
  const char* tag = ok ? "[ok]  " : "[FAIL]";
  std::printf("%s %s\n", tag, msg.c_str());
  report += std::string(tag) + " " + msg + "\n";
  if (!ok) {
    allOk = false;
  }
}

void check(bool cond, const std::string& msg, std::string& report, bool& allOk) {
  line(cond, msg, report, allOk);
}

const char* const kVertexGlsl = R"(
#version 450
layout(location=0) in vec3 position;
void main() {
    gl_Position = vec4(position, 1.0);
}
)";

const char* const kFragmentGlsl = R"(
#version 450
layout(location=0) out vec4 fragColor;
layout(set=0, binding=0) uniform CBuf { vec4 u_color; };
void main() {
    fragColor = u_color;
}
)";

struct Vertex { float x, y, z; };

}  // namespace

int RunDrawSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  bool allOk = true;
  std::printf("=== fable2 W8 draw dispatch smoke test ===\n");

  // --- 1. Create the VkContext. ---
  std::unique_ptr<VkContext> ctx;
  std::string err;
  if (!VkContext::Create(config, ctx, err)) {
    line(false, "VkContext: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VkContext created", report, allOk);
  const VkDevice device = ctx->device();
  Functions& f = ctx->functions();

  // --- 2. Create the managers. ---
  DescriptorManager descriptor(ctx.get());
  DescriptorManagerConfig descCfg;
  if (!descriptor.Create(descCfg, err)) {
    line(false, "DescriptorManager::Create: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  PipelineManager pipelines(ctx.get(), &descriptor);
  RenderPassManager rpm(ctx.get());
  DrawDispatcher dispatcher(ctx.get());
  line(true, "managers created", report, allOk);

  // --- 3. Compile the GLSL. ---
  std::vector<uint32_t> vsSpirv, fsSpirv;
  if (!CompileGlslToSpirv(EShLangVertex, kVertexGlsl, vsSpirv, err) ||
      !CompileGlslToSpirv(EShLangFragment, kFragmentGlsl, fsSpirv, err)) {
    line(false, "GLSL: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "GLSL compiled", report, allOk);

  // --- 4. Topology mapping verification. ---
  {
    check(DrawDispatcher::MapTopology(XenosTopology::kTriangleList) ==
              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
          "topology: kTriangleList -> TRIANGLE_LIST", report, allOk);
    check(DrawDispatcher::MapTopology(XenosTopology::kTriangleStrip) ==
              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
          "topology: kTriangleStrip -> TRIANGLE_STRIP", report, allOk);
    check(DrawDispatcher::MapTopology(XenosTopology::kPointList) ==
              VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
          "topology: kPointList -> POINT_LIST", report, allOk);
    check(DrawDispatcher::MapTopology(XenosTopology::kLineList) ==
              VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
          "topology: kLineList -> LINE_LIST", report, allOk);
    check(DrawDispatcher::MapTopology(XenosTopology::kTriangleFan) ==
              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
          "topology: kTriangleFan -> TRIANGLE_LIST (pre-translated)", report, allOk);
    check(DrawDispatcher::MapTopology(XenosTopology::kQuadList) ==
              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
          "topology: kQuadList -> TRIANGLE_LIST (pre-translated)", report, allOk);
  }

  // --- 5. Allocate a 256x256 RT + render pass. ---
  const uint32_t W = 256, H = 256;
  VkImage rtt;
  VmaAllocation rttAlloc;
  VkImageView rttView;
  {
    VkImageCreateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cbi.imageType = VK_IMAGE_TYPE_2D;
    cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
    cbi.extent = {W, H, 1};
    cbi.mipLevels = 1;
    cbi.arrayLayers = 1;
    cbi.samples = VK_SAMPLE_COUNT_1_BIT;
    cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
    cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ctx->AllocateImage(&rtt, &rttAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "W8-rtt", err)) {
      line(false, "RTT alloc: " + err, report, allOk);
      std::printf("%s", report.c_str());
      return 1;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = rtt;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCreateImageView(device, &vi, nullptr, &rttView);
  }

  RenderPassKey rpKey;
  rpKey.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  rpKey.colorSamples = VK_SAMPLE_COUNT_1_BIT;
  rpKey.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  rpKey.colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  PassLayout layout = rpm.GetPass(rpKey);
  check(layout.pass != VK_NULL_HANDLE, "render pass created", report, allOk);

  // --- 6. Pipelines for each topology. ---
  auto makePipeline = [&](VkPrimitiveTopology topo) -> VkPipeline {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = topo;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;
    return pipelines.GetPipeline(pk, layout.pass, vsSpirv, fsSpirv);
  };
  VkPipeline pipeTriList = makePipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  VkPipeline pipeTriStrip = makePipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  check(pipeTriList != VK_NULL_HANDLE && pipeTriStrip != VK_NULL_HANDLE,
        "pipelines created", report, allOk);

  // --- 7. Vertex buffer (4 vertices: enough for a triangle or a quad strip). ---
  const Vertex kQuadVerts[4] = {
      {-0.5f, -0.5f, 0.0f},
      {0.5f, -0.5f, 0.0f},
      {-0.5f, 0.5f, 0.0f},
      {0.5f, 0.5f, 0.0f},
  };
  VkBuffer vbuf;
  VmaAllocation vbufAlloc;
  void* vmap = nullptr;
  if (!ctx->AllocateBuffer(&vbuf, &vbufAlloc, sizeof(kQuadVerts), VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &vmap, err)) {
    line(false, "vertex buffer: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  std::memcpy(vmap, kQuadVerts, sizeof(kQuadVerts));
  {
    VmaAllocationInfo vinfo{};
    vmaGetAllocationInfo(ctx->vma(), vbufAlloc, &vinfo);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = vinfo.deviceMemory;
    r.offset = 0;
    r.size = sizeof(kQuadVerts);
    f.vkFlushMappedMemoryRanges(device, 1, &r);
  }

  // Descriptor set with a red cbuffer.
  descriptor.BeginFrame();
  VkDescriptorSet dset = descriptor.AllocShaderSet();
  // Red as float bits: R=1.0 (0x3F800000), G=0, B=0, A=1.0.
  uint32_t cbuf[4] = {0x3F800000u, 0x00000000u, 0x00000000u, 0x3F800000u};
  VkBuffer cbufHandle;
  VkDeviceSize cbufOffset;
  descriptor.StageConstants(cbuf, sizeof(cbuf), &cbufHandle, &cbufOffset);
  descriptor.WriteUniform(dset, 0, cbufHandle, cbufOffset, sizeof(cbuf));

  // Readback buffer.
  void* readbackMap = nullptr;
  VkBuffer readbackBuf;
  VmaAllocation readbackAlloc;
  ctx->AllocateBuffer(&readbackBuf, &readbackAlloc, W * H * 4, VMA_MEMORY_USAGE_GPU_TO_CPU,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &readbackMap, err);

  // Full-screen viewport + scissor.
  ViewportState fullVp{0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f};
  ScissorState fullSc{0, 0, W, H};

  // Helper: check center pixel.
  auto checkCenter = [&](const char* label) {
    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = static_cast<const uint8_t*>(readbackMap) + (cy * W + cx) * 4;
    check(px[0] >= 200 && px[1] < 60 && px[2] < 60,
          std::string(label) + " (center RGB = " + std::to_string(px[0]) + "," +
              std::to_string(px[1]) + "," + std::to_string(px[2]) + ")",
          report, allOk);
  };

  // Helper: flush the vertex buffer after a rewrite.
  auto flushVbuf = [&](size_t size) {
    VmaAllocationInfo vinfo{};
    vmaGetAllocationInfo(ctx->vma(), vbufAlloc, &vinfo);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = vinfo.deviceMemory;
    r.offset = 0;
    r.size = size;
    f.vkFlushMappedMemoryRanges(device, 1, &r);
  };

  // Helper: do a non-indexed draw with the given pipeline + params, read back.
  auto doDraw = [&](VkPipeline pipe, const DrawParams& dp) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pbi{};
    pbi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pbi.queueFamilyIndex = ctx->caps().graphicsFamily;
    f.vkCreateCommandPool(device, &pbi, nullptr, &pool);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    f.vkAllocateCommandBuffers(device, &abi, &cb);
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    f.vkBeginCommandBuffer(cb, &bbi);

    VkFramebuffer fb =
        rpm.GetFramebuffer(layout.pass, W, H, std::vector<VkImageView>{rttView});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    VkClearValue cv{};
    cv.color.float32[0] = 0.0f;
    cv.color.float32[1] = 0.0f;
    cv.color.float32[2] = 0.0f;
    cv.color.float32[3] = 1.0f;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
    const uint32_t binding = 0;
    const uint64_t voffset = 0;
    f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);
    dispatcher.Draw(cb, dp);
    f.vkCmdEndRenderPass(cb);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.image = rtt;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rbr{};
    rbr.bufferOffset = 0;
    rbr.bufferRowLength = 0;
    rbr.bufferImageHeight = H;
    rbr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rbr.imageOffset = {0, 0, 0};
    rbr.imageExtent = {W, H, 1};
    f.vkCmdCopyImageToBuffer(cb, rtt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuf, 1, &rbr);

    f.vkEndCommandBuffer(cb);
    VkFence fence;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f.vkCreateFence(device, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
    f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);
  };

  // --- 8. Non-indexed triangle list (centered triangle). ---
  {
    // Write a centered triangle that definitely covers (0,0).
    const Vertex kCenteredTri[3] = {
        {-0.5f, -0.5f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    std::memcpy(vmap, kCenteredTri, sizeof(kCenteredTri));
    flushVbuf(sizeof(kCenteredTri));
    DrawParams dp;
    dp.topology = XenosTopology::kTriangleList;
    dp.vertexCount = 3;
    dp.viewport = fullVp;
    dp.scissor = fullSc;
    dp.scissorEnabled = true;
    doDraw(pipeTriList, dp);
    checkCenter("non-indexed triangle list: red drawn");
  }

  // --- 9. Triangle strip (4 verts = 2 triangles, full quad). ---
  {
    std::memcpy(vmap, kQuadVerts, sizeof(kQuadVerts));
    flushVbuf(sizeof(kQuadVerts));
    DrawParams dp;
    dp.topology = XenosTopology::kTriangleStrip;
    dp.vertexCount = 4;
    dp.viewport = fullVp;
    dp.scissor = fullSc;
    dp.scissorEnabled = true;
    doDraw(pipeTriStrip, dp);
    checkCenter("triangle strip: red drawn");
  }

  // --- 10. Indexed draw (16-bit). ---
  {
    std::memcpy(vmap, kQuadVerts, sizeof(kQuadVerts));
    flushVbuf(sizeof(kQuadVerts));
    const uint16_t kIdx16[6] = {0, 1, 2, 2, 1, 3};

    VkBuffer ibuf;
    VmaAllocation ibufAlloc;
    void* imap = nullptr;
    ctx->AllocateBuffer(&ibuf, &ibufAlloc, sizeof(kIdx16), VMA_MEMORY_USAGE_CPU_TO_GPU,
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &imap, err);
    std::memcpy(imap, kIdx16, sizeof(kIdx16));
    {
      VmaAllocationInfo iinfo{};
      vmaGetAllocationInfo(ctx->vma(), ibufAlloc, &iinfo);
      VkMappedMemoryRange r{};
      r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      r.memory = iinfo.deviceMemory;
      r.offset = 0;
      r.size = sizeof(kIdx16);
      f.vkFlushMappedMemoryRanges(device, 1, &r);
    }

    VkCommandPool pool;
    VkCommandPoolCreateInfo pbi{};
    pbi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pbi.queueFamilyIndex = ctx->caps().graphicsFamily;
    f.vkCreateCommandPool(device, &pbi, nullptr, &pool);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    f.vkAllocateCommandBuffers(device, &abi, &cb);
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    f.vkBeginCommandBuffer(cb, &bbi);

    VkFramebuffer fb =
        rpm.GetFramebuffer(layout.pass, W, H, std::vector<VkImageView>{rttView});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    VkClearValue cv{};
    cv.color.float32[0] = 0.0f;
    cv.color.float32[1] = 0.0f;
    cv.color.float32[2] = 0.0f;
    cv.color.float32[3] = 1.0f;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeTriList);
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
    const uint32_t binding = 0;
    const uint64_t voffset = 0;
    f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);
    dispatcher.BindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);
    DrawParams dp;
    dp.topology = XenosTopology::kTriangleList;
    dp.indexed = true;
    dp.indexCount = 6;
    dp.viewport = fullVp;
    dp.scissor = fullSc;
    dp.scissorEnabled = true;
    dispatcher.DrawIndexed(cb, dp, VK_INDEX_TYPE_UINT16);
    f.vkCmdEndRenderPass(cb);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.image = rtt;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rbr{};
    rbr.bufferOffset = 0;
    rbr.bufferRowLength = 0;
    rbr.bufferImageHeight = H;
    rbr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rbr.imageOffset = {0, 0, 0};
    rbr.imageExtent = {W, H, 1};
    f.vkCmdCopyImageToBuffer(cb, rtt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuf, 1, &rbr);

    f.vkEndCommandBuffer(cb);
    VkFence fence;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f.vkCreateFence(device, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
    f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);

    checkCenter("indexed draw (16-bit): red drawn");
    check(dispatcher.drawIndexedCalls >= 1, "indexed draw call issued", report, allOk);
    ctx->FreeBuffer(ibuf, ibufAlloc);
  }

  // --- 11. Scissor clipping (left half red, right half black). ---
  {
    // Write a full-screen quad (-1.0 to 1.0) so the scissor clips it.
    const Vertex kFullScreen[4] = {
        {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
        {-1.0f, 1.0f, 0.0f},  {1.0f, 1.0f, 0.0f},
    };
    std::memcpy(vmap, kFullScreen, sizeof(kFullScreen));
    flushVbuf(sizeof(kFullScreen));
    DrawParams dp;
    dp.topology = XenosTopology::kTriangleStrip;
    dp.vertexCount = 4;
    dp.viewport = fullVp;
    dp.scissor = {0, 0, W / 2, H};
    dp.scissorEnabled = true;
    doDraw(pipeTriStrip, dp);

    const uint8_t* rd = static_cast<const uint8_t*>(readbackMap);
    const uint32_t cy = H / 2;
    const uint8_t* leftPx = rd + (cy * W + W / 4) * 4;
    const uint8_t* rightPx = rd + (cy * W + 3 * W / 4) * 4;
    check(leftPx[0] >= 200 && leftPx[1] < 60 && leftPx[2] < 60,
          "scissor: left half is red (RGB = " + std::to_string(leftPx[0]) + "," +
              std::to_string(leftPx[1]) + "," + std::to_string(leftPx[2]) + ")",
          report, allOk);
    check(rightPx[0] < 60 && rightPx[1] < 60 && rightPx[2] < 60,
          "scissor: right half is black (RGB = " + std::to_string(rightPx[0]) + "," +
              std::to_string(rightPx[1]) + "," + std::to_string(rightPx[2]) + ")",
          report, allOk);
  }

  // --- 12. Draw call counts. ---
  {
    check(dispatcher.drawCalls >= 2, "draw calls issued (" +
              std::to_string(dispatcher.drawCalls) + " non-indexed)", report, allOk);
    check(dispatcher.drawIndexedCalls >= 1, "indexed draw calls issued (" +
              std::to_string(dispatcher.drawIndexedCalls) + " indexed)", report, allOk);
    check(dispatcher.viewportSets >= 2, "viewport sets issued (" +
              std::to_string(dispatcher.viewportSets) + ")", report, allOk);
  }

  // --- Teardown. ---
  ctx->FreeBuffer(vbuf, vbufAlloc);
  ctx->FreeBuffer(readbackBuf, readbackAlloc);
  ctx->FreeImage(rtt, rttAlloc);
  f.vkDestroyImageView(device, rttView, nullptr);
  // Destroy the managers before the context (they hold references to it).
  dispatcher = DrawDispatcher(ctx.get());
  rpm.DestroyAll();
  pipelines.DestroyAll();
  descriptor.DestroyAll();
  line(true, "teardown clean", report, allOk);

  ctx.reset();

  const bool pass = allOk;
  std::printf("\n=== fable2 W8 draw dispatch smoke test: %s ===\n",
              pass ? "PASS" : "FAIL");
  if (!reportFile.empty()) {
    FILE* fp = std::fopen(reportFile.c_str(), "w");
    if (fp) {
      std::fputs(report.c_str(), fp);
      std::fclose(fp);
    }
  }
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
