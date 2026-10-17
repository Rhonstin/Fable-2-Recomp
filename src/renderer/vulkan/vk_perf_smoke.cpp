// W10 — Performance profiler smoke test.
//
// Verifies the FrameProfiler correctly tracks per-frame timing,
// draw/descriptor/pipeline call counts, cache hit rates, and
// rolling statistics over multiple frames.
#include "vk_perf_smoke.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/Initialize.h>
#include <glslang/Public/ShaderLang.h>

#include "descriptor_manager.h"
#include "draw_dispatch.h"
#include "frame_profiler.h"
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
    r.maxVertexOutputComponents = 64;
    r.maxFragmentInputComponents = 128;
    r.maxClipDistances = 8;
    r.maxViewports = 16;
    r.maxFragmentImageUniforms = 8;
    r.maxGeometryInputComponents = 64;
    r.maxGeometryOutputComponents = 128;
    r.maxGeometryTextureImageUnits = 32;
    r.maxGeometryOutputVertices = 256;
    r.maxGeometryTotalOutputComponents = 1024;
    r.maxGeometryUniformComponents = 4096;
    r.maxGeometryVaryingComponents = 64;
    r.maxTessControlInputComponents = 64;
    r.maxTessControlOutputComponents = 64;
    r.maxTessControlTextureImageUnits = 32;
    r.maxTessControlUniformComponents = 4096;
    r.maxTessControlTotalOutputComponents = 1024;
    r.maxTessEvaluationInputComponents = 64;
    r.maxTessEvaluationOutputComponents = 64;
    r.maxTessEvaluationTextureImageUnits = 32;
    r.maxTessEvaluationUniformComponents = 4096;
    r.maxPatchVertices = 32;
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
  if (!ok) allOk = false;
}

void check(bool cond, const std::string& msg, std::string& report, bool& allOk) {
  line(cond, msg, report, allOk);
}

struct Vertex { float x, y, z; };

const char* const kVsGlsl = R"(
#version 450
layout(location=0) in vec3 position;
void main() {
    gl_Position = vec4(position, 1.0);
}
)";

const char* const kFsGlsl = R"(
#version 450
layout(location=0) out vec4 fragColor;
layout(set=0, binding=0) uniform CBuf { vec4 u_color; };
void main() {
    fragColor = u_color;
}
)";

}  // namespace

int RunPerfSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  bool allOk = true;
  std::printf("=== fable2 W10 performance profiler smoke test ===\n");

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
  FrameProfiler profiler;
  line(true, "managers + profiler created", report, allOk);

  // --- 3. Compile shaders. ---
  std::vector<uint32_t> vsSpirv, fsSpirv;
  if (!CompileGlslToSpirv(EShLangVertex, kVsGlsl, vsSpirv, err) ||
      !CompileGlslToSpirv(EShLangFragment, kFsGlsl, fsSpirv, err)) {
    line(false, "GLSL: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "GLSL compiled", report, allOk);

  // --- 4. Allocate RT + render pass + pipeline. ---
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
    cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ctx->AllocateImage(&rtt, &rttAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "W10-rtt", err)) {
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

  PipelineKey pk;
  pk.vertexInput = VertexInputLayout::kPos;
  pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  pk.cullMode = VK_CULL_MODE_NONE;
  pk.depthTest = false;
  pk.samples = VK_SAMPLE_COUNT_1_BIT;
  VkPipeline pipe = pipelines.GetPipeline(pk, layout.pass, vsSpirv, fsSpirv);
  check(pipe != VK_NULL_HANDLE, "pipeline created", report, allOk);

  // --- 5. Vertex buffer: centered triangle. ---
  const Vertex kTri[3] = {
      {-0.5f, -0.5f, 0.0f},
      { 0.5f, -0.5f, 0.0f},
      { 0.0f,  0.5f, 0.0f},
  };
  VkBuffer vbuf;
  VmaAllocation vbufAlloc;
  void* vmap = nullptr;
  if (!ctx->AllocateBuffer(&vbuf, &vbufAlloc, sizeof(kTri), VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &vmap, err)) {
    line(false, "vertex buffer: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  std::memcpy(vmap, kTri, sizeof(kTri));
  {
    VmaAllocationInfo vinfo{};
    vmaGetAllocationInfo(ctx->vma(), vbufAlloc, &vinfo);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = vinfo.deviceMemory;
    r.offset = 0;
    r.size = sizeof(kTri);
    f.vkFlushMappedMemoryRanges(device, 1, &r);
  }

  // Descriptor set with a red cbuffer.
  descriptor.BeginFrame();
  VkDescriptorSet dset = descriptor.AllocShaderSet();
  uint32_t cbuf[4] = {0x3F800000u, 0x00000000u, 0x00000000u, 0x3F800000u};
  VkBuffer cbufHandle;
  VkDeviceSize cbufOffset;
  descriptor.StageConstants(cbuf, sizeof(cbuf), &cbufHandle, &cbufOffset);
  descriptor.WriteUniform(dset, 0, cbufHandle, cbufOffset, sizeof(cbuf));

  // --- 6. Run 10 frames, each with 3 draws. ---
  const int kNumFrames = 10;
  const int kDrawsPerFrame = 3;

  ViewportState fullVp{0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f};
  ScissorState fullSc{0, 0, W, H};
  DrawParams dp;
  dp.topology = XenosTopology::kTriangleList;
  dp.vertexCount = 3;
  dp.viewport = fullVp;
  dp.scissor = fullSc;
  dp.scissorEnabled = true;

  for (int frame = 0; frame < kNumFrames; ++frame) {
    profiler.BeginFrame();
    profiler.SetGuestFrameTime(16.67);  // Simulate 60 Hz guest frame.

    // Record a command buffer with 3 draws.
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
    profiler.Current().commandBuffersRecorded++;

    VkFramebuffer fb = rpm.GetFramebuffer(layout.pass, W, H, std::vector<VkImageView>{rttView});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    VkClearValue cv{};
    cv.color.float32[3] = 1.0f;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    profiler.Current().pipelineBindings++;
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
    const uint32_t binding = 0;
    const uint64_t voffset = 0;
    f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);

    for (int d = 0; d < kDrawsPerFrame; ++d) {
      dispatcher.Draw(cb, dp);
      profiler.Current().drawCalls++;
    }

    f.vkCmdEndRenderPass(cb);
    f.vkEndCommandBuffer(cb);

    // Submit + wait.
    VkFence fence;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f.vkCreateFence(device, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
    profiler.Current().queueSubmits++;
    f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
    profiler.Current().fenceWaits++;
    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);

    profiler.EndFrame();
  }

  // --- 7. Verify profiler results. ---
  check(profiler.FrameCount() == kNumFrames,
        "frame count = " + std::to_string(profiler.FrameCount()), report, allOk);

  const RollingStats& rs = profiler.Rolling();
  check(rs.frameCount == kNumFrames,
        "rolling frame count = " + std::to_string(rs.frameCount), report, allOk);
  check(rs.totalDrawCalls == (uint64_t)(kNumFrames * kDrawsPerFrame),
        "total draws = " + std::to_string(rs.totalDrawCalls) +
            " (expected " + std::to_string(kNumFrames * kDrawsPerFrame) + ")",
        report, allOk);
  check(rs.avgDrawsPerFrame > 2.9 && rs.avgDrawsPerFrame < 3.1,
        "avg draws/frame = " + std::to_string(rs.avgDrawsPerFrame) + " (expected ~3.0)",
        report, allOk);
  check(rs.totalPipelineBindings == (uint64_t)kNumFrames,
        "total pipeline bindings = " + std::to_string(rs.totalPipelineBindings),
        report, allOk);
  check(rs.avgCpuMs > 0.0 && rs.avgCpuMs < 50.0,
        "avg CPU encode = " + std::to_string(rs.avgCpuMs) + " ms (reasonable range)",
        report, allOk);
  check(rs.avgGuestMs > 16.0 && rs.avgGuestMs < 17.0,
        "avg guest frame = " + std::to_string(rs.avgGuestMs) + " ms (expected ~16.67)",
        report, allOk);

  // Last frame stats.
  const FrameStats& last = profiler.LastFrame();
  check(last.drawCalls == kDrawsPerFrame,
        "last frame draws = " + std::to_string(last.drawCalls), report, allOk);
  check(last.pipelineBindings == 1,
        "last frame pipeline bindings = " + std::to_string(last.pipelineBindings), report, allOk);
  check(last.queueSubmits == 1,
        "last frame submits = " + std::to_string(last.queueSubmits), report, allOk);
  check(last.fenceWaits == 1,
        "last frame fence waits = " + std::to_string(last.fenceWaits), report, allOk);
  check(last.commandBuffersRecorded == 1,
        "last frame cmd buffers = " + std::to_string(last.commandBuffersRecorded), report, allOk);
  check(last.cpuEncodeMs > 0.0,
        "last frame CPU encode = " + std::to_string(last.cpuEncodeMs) + " ms (> 0)",
        report, allOk);
  check(last.guestFrameMs > 16.0,
        "last frame guest time = " + std::to_string(last.guestFrameMs) + " ms",
        report, allOk);

  // Pipeline cache stats.
  check(last.pipelineCreates == 0,
        "last frame pipeline creates = 0 (cached)", report, allOk);

  // --- 8. Test profiler reset. ---
  profiler.Reset();
  check(profiler.FrameCount() == 0, "reset: frame count = 0", report, allOk);
  check(profiler.Rolling().frameCount == 0, "reset: rolling frame count = 0", report, allOk);

  // --- 9. Print report. ---
  // Run 2 more frames for the report.
  for (int frame = 0; frame < 2; ++frame) {
    profiler.BeginFrame();
    profiler.SetGuestFrameTime(16.67);
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
    profiler.Current().commandBuffersRecorded++;
    VkFramebuffer fb = rpm.GetFramebuffer(layout.pass, W, H, std::vector<VkImageView>{rttView});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    VkClearValue cv{};
    cv.color.float32[3] = 1.0f;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    profiler.Current().pipelineBindings++;
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
    const uint32_t binding = 0;
    const uint64_t voffset = 0;
    f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);
    for (int d = 0; d < kDrawsPerFrame; ++d) {
      dispatcher.Draw(cb, dp);
      profiler.Current().drawCalls++;
    }
    f.vkCmdEndRenderPass(cb);
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
    profiler.Current().queueSubmits++;
    f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
    profiler.Current().fenceWaits++;
    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);
    profiler.EndFrame();
  }
  profiler.Report("W10 PerfProfiler");

  // --- Teardown. ---
  f.vkDestroyImageView(device, rttView, nullptr);
  pipelines.DestroyAll();
  descriptor.DestroyAll();
  rpm.DestroyAll();
  ctx->FreeBuffer(vbuf, vbufAlloc);
  ctx->FreeImage(rtt, rttAlloc);
  line(true, "teardown clean", report, allOk);

  // Write report.
  {
    FILE* fp = fopen(reportFile.c_str(), "w");
    if (fp) { fwrite(report.c_str(), 1, report.size(), fp); fclose(fp); }
  }

  std::printf("=== W10 %s ===\n", allOk ? "PASS" : "FAIL");
  return allOk ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
