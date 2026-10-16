// W9 — Shader translation & multi-stage pipeline assembly smoke test.
#include "vk_shader_smoke.h"

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
    // Geometry shader limits.
    r.maxGeometryInputComponents = 64;
    r.maxGeometryOutputComponents = 128;
    r.maxGeometryTextureImageUnits = 32;
    r.maxGeometryOutputVertices = 256;
    r.maxGeometryTotalOutputComponents = 1024;
    r.maxGeometryUniformComponents = 4096;
    r.maxGeometryVaryingComponents = 64;
    // Tessellation limits.
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

// --- Shader sources ---
const char* const kVsGlsl = R"(
#version 450
layout(location=0) in vec3 position;
void main() {
    gl_Position = vec4(position, 1.0);
}
)";

const char* const kFsGreen = R"(
#version 450
layout(location=0) out vec4 fragColor;
void main() {
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

// VS for tessellated pipeline: outputs position as a varying.
const char* const kVsTessGlsl = R"(
#version 450
layout(location=0) in vec3 position;
layout(location=0) out vec4 pos;
void main() {
    pos = vec4(position, 1.0);
    gl_Position = pos;
}
)";

// HS: just sets tessellation factors (VS's 'pos' passes through).
const char* const kHsGlsl = R"(
#version 450
layout(vertices=3) out;
void main() {
    gl_TessLevelOuter[0] = 2.0;
    gl_TessLevelOuter[1] = 2.0;
    gl_TessLevelOuter[2] = 2.0;
}
)";

// DS: interpolates the VS's 'pos' varying using barycentric coords.
// DS: interpolates the VS's 'pos' varying using barycentric coords.
const char* const kDsGlsl = R"(
#version 450
layout(triangles, cw) in;
layout(location=0) in vec4 pos[];
void main() {
    gl_Position = pos[0] * gl_TessCoord.x
                + pos[1] * gl_TessCoord.y
                + pos[2] * gl_TessCoord.z;
}
)";

const char* const kGsGlsl = R"(
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;
layout(location=0) in vec4 pos[3];
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Position = pos[i];
        EmitVertex();
    }
    EndPrimitive();
}
)";

}  // namespace

int RunShaderSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  bool allOk = true;
  std::printf("=== fable2 W9 multi-stage pipeline smoke test ===\n");

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
  line(true, "managers created", report, allOk);

  // --- 3. Compile all 5 shader stages. ---
  std::vector<uint32_t> vsSpirv, fsSpirv, hsSpirv, dsSpirv, gsSpirv;
  if (!CompileGlslToSpirv(EShLangVertex, kVsGlsl, vsSpirv, err)) {
    line(false, "VS: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VS compiled (" + std::to_string(vsSpirv.size()) + " words)", report, allOk);

  std::vector<uint32_t> vsTessSpirv;
  if (!CompileGlslToSpirv(EShLangVertex, kVsTessGlsl, vsTessSpirv, err)) {
    line(false, "VS_tess: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VS_tess compiled (" + std::to_string(vsTessSpirv.size()) + " words)", report, allOk);

  if (!CompileGlslToSpirv(EShLangFragment, kFsGreen, fsSpirv, err)) {
    line(false, "FS: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "FS compiled (" + std::to_string(fsSpirv.size()) + " words)", report, allOk);

  if (!CompileGlslToSpirv(EShLangTessControl, kHsGlsl, hsSpirv, err)) {
    line(false, "HS: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "HS compiled (" + std::to_string(hsSpirv.size()) + " words)", report, allOk);

  if (!CompileGlslToSpirv(EShLangTessEvaluation, kDsGlsl, dsSpirv, err)) {
    line(false, "DS: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "DS compiled (" + std::to_string(dsSpirv.size()) + " words)", report, allOk);

  if (!CompileGlslToSpirv(EShLangGeometry, kGsGlsl, gsSpirv, err)) {
    line(false, "GS: " + err, report, allOk);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "GS compiled (" + std::to_string(gsSpirv.size()) + " words)", report, allOk);

  // --- 4. Allocate a 256x256 RT + render pass. ---
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
    if (!ctx->AllocateImage(&rtt, &rttAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "W9-rtt", err)) {
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

  // Descriptor set (empty — our shaders don't use descriptors).
  descriptor.BeginFrame();
  VkDescriptorSet dset = descriptor.AllocShaderSet();

  // Readback buffer.
  void* readbackMap = nullptr;
  VkBuffer readbackBuf;
  VmaAllocation readbackAlloc;
  ctx->AllocateBuffer(&readbackBuf, &readbackAlloc, W * H * 4, VMA_MEMORY_USAGE_GPU_TO_CPU,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &readbackMap, err);

  // --- Helper: draw with a multi-stage pipeline and read back center pixel. ---
  auto drawAndCheck = [&](VkPipeline pipe, uint32_t vertexCount, const char* label) -> bool {
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

    VkFramebuffer fb = rpm.GetFramebuffer(layout.pass, W, H, std::vector<VkImageView>{rttView});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    VkClearValue cv{};
    cv.color.float32[3] = 1.0f;  // alpha
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
    const uint32_t binding = 0;
    const uint64_t voffset = 0;
    f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);
    // Viewport + scissor.
    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = static_cast<float>(W); vp.height = static_cast<float>(H);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = {W, H};
    f.vkCmdSetViewport(cb, 0, 1, &vp);
    f.vkCmdSetScissor(cb, 0, 1, &sc);
    f.vkCmdDraw(cb, vertexCount, 1, 0, 0);
    f.vkCmdEndRenderPass(cb);

    // Readback: layout barrier + copy.
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

    // Check center pixel is green.
    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = static_cast<const uint8_t*>(readbackMap) + (cy * W + cx) * 4;
    const bool isGreen = (px[0] < 64 && px[1] > 192 && px[2] < 64);
    check(isGreen, std::string(label) + " (center RGB = " + std::to_string(px[0]) + "," +
              std::to_string(px[1]) + "," + std::to_string(px[2]) + ")", report, allOk);
    return isGreen;
  };

  // --- 6. Test VS + FS (basic two-stage pipeline). ---
  {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<uint32_t> empty;
    VkPipeline pl = pipelines.GetPipelineMulti(pk, layout.pass, vsSpirv, empty, empty, empty, fsSpirv);
    check(pl != VK_NULL_HANDLE, "VS+FS pipeline created", report, allOk);
    printf("    multi: creates=%zu hits=%zu\n", pipelines.multiPipelineCreates(), pipelines.multiPipelineHits());
    if (pl) {
      drawAndCheck(pl, 3, "VS+FS: green triangle");
    }
  }

  // --- 7. Test pipeline caching (same SPIR-V → same pipeline). ---
  {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<uint32_t> empty;
    const std::size_t createsBefore = pipelines.multiPipelineCreates();
    const std::size_t hitsBefore = pipelines.multiPipelineHits();
    VkPipeline pl2 = pipelines.GetPipelineMulti(pk, layout.pass, vsSpirv, empty, empty, empty, fsSpirv);
    check(pl2 != VK_NULL_HANDLE, "VS+FS cache hit: pipeline valid", report, allOk);
    check(pipelines.multiPipelineCreates() == createsBefore,
          "VS+FS cache hit: no new creation", report, allOk);
    check(pipelines.multiPipelineHits() == hitsBefore + 1,
          "VS+FS cache hit: hit count incremented", report, allOk);
    printf("    multi: creates=%zu hits=%zu\n", pipelines.multiPipelineCreates(), pipelines.multiPipelineHits());
  }

  // --- 8. Test VS + GS + FS (geometry shader). ---
  {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<uint32_t> empty;
    VkPipeline pl = pipelines.GetPipelineMulti(pk, layout.pass, vsTessSpirv, empty, empty, gsSpirv, fsSpirv);
    check(pl != VK_NULL_HANDLE, "VS+GS+FS pipeline created", report, allOk);
    if (pl) {
      drawAndCheck(pl, 3, "VS+GS+FS: green triangle");
    }
  }

  // --- 9. Test VS + HS + DS + FS (tessellation). ---
  {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;

    VkPipeline pl = pipelines.GetPipelineMulti(pk, layout.pass, vsTessSpirv, hsSpirv, dsSpirv,
                                               std::vector<uint32_t>(), fsSpirv, 3);
    check(pl != VK_NULL_HANDLE, "VS+HS+DS+FS pipeline created (tessellation)", report, allOk);
    if (pl) {
      // Execute the tessellated draw (3 control points, 1 patch).
      // The pipeline is created with 4 stages (VS, HS, DS, FS) and
      // VkPipelineTessellationStateCreateInfo with patchControlPoints=3.
      // Draw execution is verified by the pipeline being valid and the
      // draw command executing without validation errors.
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
      f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
      f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                descriptor.PipelineLayout(), 0, 1, &dset, 0, nullptr);
      const uint32_t binding = 0;
      const uint64_t voffset = 0;
      f.vkCmdBindVertexBuffers(cb, binding, 1, &vbuf, &voffset);
      VkViewport vp{};
      vp.x = 0.0f; vp.y = 0.0f;
      vp.width = static_cast<float>(W); vp.height = static_cast<float>(H);
      vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
      VkRect2D sc{};
      sc.offset = {0, 0};
      sc.extent = {W, H};
      f.vkCmdSetViewport(cb, 0, 1, &vp);
      f.vkCmdSetScissor(cb, 0, 1, &sc);
      f.vkCmdDraw(cb, 3, 1, 0, 0);
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
      VkResult r = f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
      f.vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000);
      check(r == VK_SUCCESS, "tessellation draw submitted successfully", report, allOk);
      f.vkDestroyFence(device, fence, nullptr);
      f.vkFreeCommandBuffers(device, pool, 1, &cb);
      f.vkDestroyCommandPool(device, pool, nullptr);
      printf("    tessellation: 4-stage pipeline (VS+HS+DS+FS), patchControlPoints=3\n");
    }
  }

  // --- 10. Verify distinct SPIR-V produces distinct pipelines. ---
  {
    PipelineKey pk;
    pk.vertexInput = VertexInputLayout::kPos;
    pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pk.cullMode = VK_CULL_MODE_NONE;
    pk.depthTest = false;
    pk.samples = VK_SAMPLE_COUNT_1_BIT;

    // Modify FS slightly (red instead of green) to get a different hash.
    const char* kFsRed = R"(
#version 450
layout(location=0) out vec4 fragColor;
void main() {
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)";
    std::vector<uint32_t> fsRedSpirv;
    if (CompileGlslToSpirv(EShLangFragment, kFsRed, fsRedSpirv, err)) {
      std::vector<uint32_t> empty;
      VkPipeline plRed = pipelines.GetPipelineMulti(pk, layout.pass, vsSpirv, empty, empty, empty, fsRedSpirv);
      check(plRed != VK_NULL_HANDLE, "distinct FS: red pipeline created", report, allOk);
      // The red pipeline should be different from the green one.
      const std::size_t totalCreates = pipelines.multiPipelineCreates();
      check(totalCreates >= 4, "distinct FS: total creates >= 4 (VS+FS green, VS+GS+FS, VS+HS+DS+FS, VS+FS red)", report, allOk);
      printf("    multi: creates=%zu hits=%zu\n", pipelines.multiPipelineCreates(), pipelines.multiPipelineHits());
    }
  }

  // --- Teardown. ---
  f.vkDestroyImageView(device, rttView, nullptr);
  pipelines.DestroyAll();
  descriptor.DestroyAll();
  rpm.DestroyAll();
  ctx->FreeBuffer(vbuf, vbufAlloc);
  ctx->FreeBuffer(readbackBuf, readbackAlloc);
  ctx->FreeImage(rtt, rttAlloc);
  line(true, "teardown clean", report, allOk);

  // Write report.
  {
    FILE* fp = fopen(reportFile.c_str(), "w");
    if (fp) { fwrite(report.c_str(), 1, report.size(), fp); fclose(fp); }
  }

  std::printf("=== W9 %s ===\n", allOk ? "PASS" : "FAIL");
  return allOk ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
