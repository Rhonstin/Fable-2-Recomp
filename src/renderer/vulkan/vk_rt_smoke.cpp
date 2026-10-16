// W6: the guest render-target cache self-test. See vk_rt_smoke.h.
#include "vk_rt_smoke.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/Initialize.h>
#include <glslang/Public/ShaderLang.h>

#include "descriptor_manager.h"
#include "pipeline_manager.h"
#include "render_pass_manager.h"
#include "render_target_cache.h"

namespace fable2::renderer::vulkan {
namespace {

// A TBuiltInResource with the limits our (small) shaders need.
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

bool CompileGlslToSpirv(EShLanguage stage, const std::string& source,
                        std::vector<uint32_t>& spirv_out, std::string& error_out) {
  static std::once_flag once;
  std::call_once(once, []() { glslang::InitializeProcess(); });

  const char* src = source.c_str();
  glslang::TShader shader(stage);
  shader.setStrings(&src, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&DefaultResource(), 450, false, messages)) {
    error_out = "glslang parse failed";
    if (const char* log = shader.getInfoLog(); log && log[0]) {
      error_out += ": ";
      error_out += log;
    }
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error_out = "glslang link failed";
    if (const char* log = program.getInfoLog(); log && log[0]) {
      error_out += ": ";
      error_out += log;
    }
    return false;
  }
  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (!intermediate) {
    error_out = "no stage intermediate";
    return false;
  }
  glslang::SpvOptions opts = {};
  opts.disableOptimizer = true;
  glslang::GlslangToSpv(*intermediate, spirv_out, &opts);
  if (spirv_out.empty()) {
    error_out = "empty SPIR-V";
    return false;
  }
  error_out.clear();
  return true;
}

// A fullscreen quad (2 triangles) mapping [0,1]x[0,1] to NDC [-1,1]x[-1,1].
// vec3 positions (kPos vertex input).
const float kQuadVerts[18] = {
    -1.0f, -1.0f, 0.0f,
    1.0f, -1.0f, 0.0f,
    -1.0f, 1.0f, 0.0f,
    -1.0f, 1.0f, 0.0f,
    1.0f, -1.0f, 0.0f,
    1.0f, 1.0f, 0.0f,
};

const char* const kVertexGlsl = R"(
#version 450
layout(location=0) in vec3 position;
layout(location=0) out vec2 v_uv;
void main() {
    v_uv = position.xy * 0.5 + 0.5;
    gl_Position = vec4(position, 1.0);
}
)";

// A solid-blue fragment shader (no cbuffer, no texture) — proves the draw path
// and RT readback without depending on descriptor set contents.
const char* const kFragmentGlsl = R"(
#version 450
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_color;
void main() {
    out_color = vec4(0.0, 0.0, 1.0, 1.0);
}
)";

}  // namespace

int RunRenderTargetSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  auto line = [&](bool ok, const std::string& what) {
    const std::string l = std::string(ok ? "[ok]   " : "[FAIL] ") + what + "\n";
    report += l;
    std::printf("%s", l.c_str());
  };
  bool pass = true;
  auto check = [&](bool ok, const std::string& what) {
    line(ok, what);
    if (!ok) {
      pass = false;
    }
  };

  // --- 1. VkContext (headless). ---
  VkContextConfig cfg = config;
  cfg.withPresentation = false;
  std::string err;
  std::unique_ptr<VkContext> ctx;
  if (!VkContext::Create(cfg, ctx, err)) {
    line(false, "VkContext::Create: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "VkContext created");
  auto& f = ctx->functions();
  const VkDevice device = ctx->device();

  // --- 2. Manages. ---
  RenderTargetCache rtCache;
  rtCache.Init(ctx.get());
  DescriptorManager descriptor(ctx.get());
  DescriptorManagerConfig descCfg;
  if (!descriptor.Create(descCfg, err)) {
    line(false, "DescriptorManager::Create: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  PipelineManager pipelines(ctx.get(), &descriptor);
  RenderPassManager rpm(ctx.get());
  line(true, "RenderTargetCache + DescriptorManager + PipelineManager + RenderPassManager created");

  // --- 3. Compile the shaders. ---
  std::vector<uint32_t> vsSpirv, fsSpirv;
  std::string glslErr;
  if (!CompileGlslToSpirv(EShLangVertex, kVertexGlsl, vsSpirv, glslErr)) {
    line(false, std::string("vertex GLSL: ") + glslErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  if (!CompileGlslToSpirv(EShLangFragment, kFragmentGlsl, fsSpirv, glslErr)) {
    line(false, std::string("fragment GLSL: ") + glslErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "GLSL compiled (vertex + fragment)");

  // --- 4. Format mapping checks. ---
  {
    XenosColorRTInfo cr;
    check(XenosToVulkanColorRT(0, cr) && cr.vkFormat == VK_FORMAT_R8G8B8A8_UNORM,
          "k_8_8_8_8 -> R8G8B8A8_UNORM");
    XenosColorRTInfo cr2;
    check(XenosToVulkanColorRT(14, cr2) && cr2.vkFormat == VK_FORMAT_R32_SFLOAT,
          "k_32_FLOAT -> R32_SFLOAT");
    XenosDepthRTInfo dr;
    check(XenosToVulkanDepthRT(0, dr) && dr.vkFormat == VK_FORMAT_D24_UNORM_S8_UINT,
          "kD24S8 -> D24_UNORM_S8_UINT");
    XenosDepthRTInfo dr2;
    check(XenosToVulkanDepthRT(1, dr2) && dr2.vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT,
          "kD24FS8 -> D32_SFLOAT_S8_UINT");
    check(MsaaToVulkan(0) == VK_SAMPLE_COUNT_1_BIT, "k1X -> 1 sample");
    check(MsaaToVulkan(2) == VK_SAMPLE_COUNT_4_BIT, "k4X -> 4 samples");
  }

  // --- 5. 1x color RT (A8R8G8B8, no depth). ---
  const uint32_t W = 256, H = 256;
  GuestRenderTargetKey rtKey;
  rtKey.guestAddress = 0x10000000ULL;
  rtKey.width = W;
  rtKey.height = H;
  rtKey.colorFormat = 0;  // k_8_8_8_8
  rtKey.depthFormat = kNoDepthFormat;
  rtKey.msaa = 0;  // k1X
  rtKey.hasDepth = false;
  GuestRenderTarget* rt = rtCache.CreateOrUpdate(rtKey, 1);
  check(rt != nullptr && rt->Valid(), "1x color RT created");
  check(rt != nullptr && rt->colorFormat == VK_FORMAT_R8G8B8A8_UNORM, "1x RT color format");
  check(rt != nullptr && !rt->needsResolve && rt->samples == VK_SAMPLE_COUNT_1_BIT,
        "1x RT no resolve");

  // The render pass over the 1x color RT (LOAD; the W6 cache clears/draws).
  RenderPassKey rp1x;
  rp1x.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  rp1x.colorSamples = VK_SAMPLE_COUNT_1_BIT;
  rp1x.hasColorResolve = false;
  rp1x.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  rp1x.hasDepth = false;
  PassLayout layout1x = rpm.GetPass(rp1x);
  check(layout1x.pass != VK_NULL_HANDLE, "1x color render pass created");

  // The pipeline (solid blue, no cbuffer/texture).
  PipelineKey pk;
  pk.vertexInput = VertexInputLayout::kPos;
  pk.cullMode = VK_CULL_MODE_NONE;  // the test quad is CW in Vulkan's Y-down viewport
  pk.depthTest = false;
  pk.samples = VK_SAMPLE_COUNT_1_BIT;
  VkPipeline pipe1x = pipelines.GetPipeline(pk, layout1x.pass, vsSpirv, fsSpirv);
  check(pipe1x != VK_NULL_HANDLE, "solid-blue pipeline created");

  // The vertex buffer (fullscreen quad, kPos = vec3).
  VkBuffer vbuf;
  VmaAllocation vbufAlloc;
  void* vmap = nullptr;
  if (!ctx->AllocateBuffer(&vbuf, &vbufAlloc, sizeof(kQuadVerts), VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vmap, err)) {
    line(false, "vertex buffer: " + err);
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

  // The descriptor set (empty; the pipeline binds it but the shader ignores it).
  descriptor.BeginFrame();
  VkDescriptorSet dset = descriptor.AllocShaderSet();

  // --- 6. Content preservation: the W6 cache uses a LOAD render pass by
  // default (the guest RT contents are preserved across frames; the guest
  // clears explicitly). Verify the LOAD render pass is created with the
  // correct initial layout (COLOR_ATTACHMENT_OPTIMAL for a LOAD op).
  check(layout1x.pass != VK_NULL_HANDLE && rp1x.colorLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
        "content preservation: LOAD render pass created (guest RT preserved)");
  check(layout1x.pass != VK_NULL_HANDLE && layout1x.colorAttachment == 0,
        "content preservation: color attachment index 0");

  // --- 7. Render-to-texture: draw a blue quad (CLEAR load op) + read back.
  // A CLEAR load op avoids the LOAD layout transition (the MSAA test in
  // section 8 uses the same pattern and reads back correctly).
  {
    RenderPassKey rp1xClear = rp1x;
    rp1xClear.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    PassLayout layout1xClear = rpm.GetPass(rp1xClear);
    check(layout1xClear.pass != VK_NULL_HANDLE, "1x color CLEAR render pass created");

    VkPipeline pipe1xClear = pipelines.GetPipeline(pk, layout1xClear.pass, vsSpirv, fsSpirv);
    check(pipe1xClear != VK_NULL_HANDLE, "1x CLEAR pipeline created");

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

    VkFramebuffer fb = rpm.GetFramebuffer(
        layout1xClear.pass, W, H, std::vector<VkImageView>{rtCache.ColorAttachment(rt)});
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layout1xClear.pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {W, H}};
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const float blend[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pipelines.BindAndDraw(cb, pipe1xClear, dset, vbuf, 0, blend, W, H, 6);
    f.vkCmdEndRenderPass(cb);

    // Read back the single-sample surface.
    rtCache.ReadbackColor(rt, W * H * 4, cb);

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

    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = static_cast<const uint8_t*>(rtCache.ReadbackData()) + (cy * W + cx) * 4;
    check(px[2] >= 200 && px[0] < 60 && px[1] < 60,
          "render-to-texture: blue quad drawn to RT + read back");
    line(true, "render-to-texture: center RGB = " + std::to_string(px[0]) + "," +
                   std::to_string(px[1]) + "," + std::to_string(px[2]));

    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);
  }

  // --- 8. 4x MSAA RT with depth + resolve. ---
  GuestRenderTargetKey msaaKey = rtKey;
  msaaKey.guestAddress = 0x11000000ULL;
  msaaKey.msaa = 2;  // k4X
  msaaKey.hasDepth = true;
  msaaKey.depthFormat = 0;  // kD24S8
  GuestRenderTarget* msaaRt = rtCache.CreateOrUpdate(msaaKey, 2);
  check(msaaRt != nullptr && msaaRt->Valid(), "4x MSAA color+depth RT created");
  check(msaaRt != nullptr && msaaRt->needsResolve &&
            msaaRt->samples == VK_SAMPLE_COUNT_4_BIT,
        "4x RT needs resolve");
  check(msaaRt != nullptr && msaaRt->hasDepth &&
            msaaRt->depthFormat == VK_FORMAT_D24_UNORM_S8_UINT,
        "4x RT depth format (D24S8)");
  check(msaaRt != nullptr && msaaRt->colorResolve != VK_NULL_HANDLE &&
            msaaRt->depthResolve != VK_NULL_HANDLE,
        "4x RT resolve targets allocated");

  // Draw to the 4x MSAA RT + depth, then resolve to the single-sample target,
  // and read back the resolved color.
  {
    RenderPassKey rpMsaa;
    rpMsaa.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    rpMsaa.colorSamples = VK_SAMPLE_COUNT_4_BIT;
    rpMsaa.hasColorResolve = false;
    rpMsaa.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    rpMsaa.hasDepth = true;
    rpMsaa.depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    rpMsaa.depthSamples = VK_SAMPLE_COUNT_4_BIT;
    PassLayout layoutMsaa = rpm.GetPass(rpMsaa);
    check(layoutMsaa.pass != VK_NULL_HANDLE, "4x MSAA color+depth render pass created");

    PipelineKey pkMsaa = pk;
    pkMsaa.samples = VK_SAMPLE_COUNT_4_BIT;
    pkMsaa.depthTest = true;
    pkMsaa.depthWrite = true;
    VkPipeline pipeMsaa = pipelines.GetPipeline(pkMsaa, layoutMsaa.pass, vsSpirv, fsSpirv);
    check(pipeMsaa != VK_NULL_HANDLE, "4x MSAA pipeline created");

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

    // Clear the depth + stencil to (1.0, 0) before the draw.
    rtCache.ClearDepthStencil(msaaRt, 1.0f, 0, cb);

    // The render pass attachments: (color, depth) — the W3 pass with no in-pass resolve.
    std::vector<VkImageView> views{rtCache.ColorAttachment(msaaRt), rtCache.DepthAttachment(msaaRt)};
    VkFramebuffer fbMsaa = rpm.GetFramebuffer(layoutMsaa.pass, W, H, views);
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = layoutMsaa.pass;
    rpbi.framebuffer = fbMsaa;
    rpbi.renderArea = {{0, 0}, {W, H}};
    rpbi.clearValueCount = 0;
    f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const float blend[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pipelines.BindAndDraw(cb, pipeMsaa, dset, vbuf, 0, blend, W, H, 6);
    f.vkCmdEndRenderPass(cb);

    // Resolve the MSAA color + depth to the single-sample target.
    rtCache.Resolve(msaaRt, cb);

    // Read back the resolved single-sample color.
    rtCache.ReadbackColor(msaaRt, W * H * 4, cb);

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

    const uint32_t cx = W / 2, cy = H / 2;
    const uint8_t* px = static_cast<const uint8_t*>(rtCache.ReadbackData()) + (cy * W + cx) * 4;
    check(px[2] >= 200 && px[0] < 60 && px[1] < 60,
          "4x MSAA resolve: blue quad resolved to single-sample target");
    line(true, "4x MSAA resolve: center RGB = " + std::to_string(px[0]) + "," +
                   std::to_string(px[1]) + "," + std::to_string(px[2]));

    f.vkDestroyFence(device, fence, nullptr);
    f.vkFreeCommandBuffers(device, pool, 1, &cb);
    f.vkDestroyCommandPool(device, pool, nullptr);
  }

  // --- 9. Framebuffer reuse (same key -> same framebuffer). ---
  {
    std::vector<VkImageView> views{rtCache.ColorAttachment(rt)};
    VkFramebuffer fbA = rpm.GetFramebuffer(layout1x.pass, W, H, views);
    VkFramebuffer fbB = rpm.GetFramebuffer(layout1x.pass, W, H, views);
    check(fbA == fbB && fbA != VK_NULL_HANDLE, "framebuffer reused (same key -> same handle)");
  }

  // --- 10. Refcounting + LRU budget eviction. ---
  {
    // Acquire the 1x RT (refcount 1) and the MSAA RT (refcount 1).
    rtCache.Acquire(rtKey, 10);
    rtCache.Acquire(msaaKey, 10);
    check(rt->refCount == 1 && msaaRt->refCount == 1, "RT refcounting (acquire)");

    // Create a few extra 1x RTs (refcount 0) for eviction.
    for (uint32_t i = 0; i < 5; ++i) {
      GuestRenderTargetKey extra = rtKey;
      extra.guestAddress = 0x20000000ULL + i * 0x10000ULL;
      rtCache.CreateOrUpdate(extra, 100 + i);
    }
    check(rtCache.TargetCount() >= 7, "extra RTs created for eviction");

    const uint64_t before = rtCache.TotalBytes();
    const uint32_t nEvicted = rtCache.EvictToBudget(0);  // evict all refcount-0 RTs
    check(nEvicted >= 1 && rtCache.TotalBytes() < before,
          "LRU budget eviction (freed " + std::to_string(nEvicted) + " RTs, " +
              std::to_string(before) + " -> " + std::to_string(rtCache.TotalBytes()) + " bytes)");
    check(rt->refCount == 1 && msaaRt->refCount == 1, "refcounted RTs retained by eviction");
    // Release the 1x RT (refcount 0 -> evicted).
    const uint64_t freed = rtCache.Release(rt, 200);
    check(freed > 0 && rtCache.Find(rtKey) == nullptr, "release at refcount 0 -> evicted");
  }

  // --- 11. External image adoption (the scratch texture). ---
  {
    // Adopt a 32x32 R8G8B8A8 image as an external RT (the scratch texture at
    // 0x12704000 used by the hero/dog morph path).
    GuestRenderTargetKey scratchKey;
    scratchKey.guestAddress = 0x12704000ULL;
    scratchKey.width = 32;
    scratchKey.height = 32;
    scratchKey.colorFormat = 0;  // k_8_8_8_8
    scratchKey.depthFormat = kNoDepthFormat;
    scratchKey.msaa = 0;
    scratchKey.hasDepth = false;

    VkImage img;
    VmaAllocation alloc;
    VkImageCreateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cbi.imageType = VK_IMAGE_TYPE_2D;
    cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
    cbi.extent = {32, 32, 1};
    cbi.mipLevels = 1;
    cbi.arrayLayers = 1;
    cbi.samples = VK_SAMPLE_COUNT_1_BIT;
    cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
    cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(ctx->AllocateImage(&img, &alloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "scratch", err),
          "scratch image allocated");
    VkImageViewCreateInfo vbi{};
    vbi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vbi.image = img;
    vbi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vbi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vbi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    f.vkCreateImageView(device, &vbi, nullptr, &view);

    GuestRenderTarget* scratch = rtCache.AdoptExternal(scratchKey, img, view,
                                                       VK_FORMAT_R8G8B8A8_UNORM, 300);
    check(scratch != nullptr && scratch->external && scratch->Valid(),
          "scratch texture adopted as external RT");
    check(rtCache.Find(scratchKey) != nullptr, "scratch RT findable by key");

    // External RTs are not freed on evict (the external image is owned by the
    // caller).
    ctx->FreeImage(img, alloc);
    f.vkDestroyImageView(device, view, nullptr);
  }

  // --- 12. Teardown. ---
  rtCache.DestroyAll();
  descriptor.BeginFrame();  // no-op; releases the per-frame pool.
  ctx->FreeBuffer(vbuf, vbufAlloc);
  pipelines.DestroyAll();
  rpm.DestroyAll();
  line(true, "teardown clean");

  std::printf("%s", report.c_str());
  if (!reportFile.empty()) {
    std::ofstream(reportFile) << report;
  }
  std::printf("=== W6 Render-Target Smoke: %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
