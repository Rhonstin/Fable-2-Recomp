// vk_pipeline_smoke.cpp — W4: descriptor sets & pipeline state self-test.
#include "vk_pipeline_smoke.h"

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <mutex>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/Initialize.h>
#include <glslang/Public/ShaderLang.h>

#include "descriptor_manager.h"
#include "pipeline_manager.h"
#include "render_pass_manager.h"

namespace fable2::renderer::vulkan {

namespace {

// A TBuiltInResource with the limits our (small) shaders need. Zero-initialized
// so no field is garbage; only the limits glslang validates against are set.
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

// GLSL -> SPIR-V via glslang (mirrors the SDK's CompileGlslToSpirvInternal).
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
  opts.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &opts);
  if (spirv_out.empty()) {
    error_out = "empty SPIR-V";
    return false;
  }
  return true;
}

// Small shaders that reference the shared descriptor set (cbuffer UBO, bindless
// texture array, canonical sampler array) so the descriptor machinery is
// actually exercised.
const char* const kVertexGlsl = R"(
#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec2 texcoord;
layout(location=0) out vec2 v_texcoord;
void main() {
  v_texcoord = texcoord;
  gl_Position = vec4(position, 1.0);
}
)";

const char* const kFragmentGlsl = R"(
#version 450
layout(location=0) in vec2 v_texcoord;
layout(set=0, binding=0) uniform CBuffer { vec4 u_color; };
layout(set=0, binding=1) uniform texture2D u_texture[];
layout(set=0, binding=2) uniform sampler u_samplers[];
layout(location=0) out vec4 out_color;
void main() {
  vec4 t = texture(sampler2D(u_texture[0], u_samplers[0]), v_texcoord);
  out_color = u_color * 0.5 + t * 0.5;
}
)";

// A single vertex: vec3 position + vec2 texcoord (stride 20 bytes).
struct Vertex {
  float x, y, z;
  float u, v;
};

}  // namespace

int RunPipelineSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  auto line = [&](bool ok, const std::string& what) {
    report += std::string(ok ? "[ok]   " : "[FAIL] ") + what + "\n";
  };

  VkContextConfig cfg = config;
  cfg.withPresentation = false;  // headless
  const std::string cachePath =
      reportFile.empty() ? "fable2-vk-pipeline-cache.bin" : reportFile + ".pcache";
  cfg.pipelineCachePath = cachePath;
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

  // --- Descriptor + render pass + pipeline managers. ---
  DescriptorManager descriptor(ctx.get());
  DescriptorManagerConfig descCfg;
  std::string descErr;
  if (!descriptor.Create(descCfg, descErr)) {
    line(false, "DescriptorManager::Create: " + descErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "DescriptorManager created (shared shader set + pipeline layout)");

  RenderPassManager rpm(ctx.get());
  RenderPassKey rpKey;  // color-only, 1x, CLEAR
  rpKey.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  PassLayout passLayout = rpm.GetPass(rpKey);
  if (passLayout.pass == VK_NULL_HANDLE) {
    line(false, "render pass create failed");
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "render pass created (color-only)");

  // --- Compile the shaders. ---
  std::vector<uint32_t> vsSpirv, fsSpirv;
  std::string shaderErr;
  if (!CompileGlslToSpirv(EShLangVertex, kVertexGlsl, vsSpirv, shaderErr)) {
    line(false, "vertex GLSL compile: " + shaderErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  if (!CompileGlslToSpirv(EShLangFragment, kFragmentGlsl, fsSpirv, shaderErr)) {
    line(false, "fragment GLSL compile: " + shaderErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "shaders compiled (vertex " + std::to_string(vsSpirv.size()) + " words, fragment " +
                 std::to_string(fsSpirv.size()) + " words)");

  // --- Render target + framebuffer. ---
  const uint32_t W = 256, H = 256;
  VkImage rt;
  VmaAllocation rtAlloc;
  VkImageCreateInfo cbi{};
  cbi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  cbi.imageType = VK_IMAGE_TYPE_2D;
  cbi.format = VK_FORMAT_R8G8B8A8_UNORM;
  cbi.extent = {W, H, 1};
  cbi.mipLevels = 1;
  cbi.arrayLayers = 1;
  cbi.samples = VK_SAMPLE_COUNT_1_BIT;
  cbi.tiling = VK_IMAGE_TILING_OPTIMAL;
  cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ctx->AllocateImage(&rt, &rtAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "pip-smoke-rt", err)) {
    line(false, "RT alloc: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  VkImageView rtView;
  VkImageViewCreateInfo ivc{};
  ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivc.image = rt;
  ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivc.format = VK_FORMAT_R8G8B8A8_UNORM;
  ivc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  f.vkCreateImageView(device, &ivc, nullptr, &rtView);
  VkFramebuffer fb = rpm.GetFramebuffer(passLayout.pass, W, H, {rtView});

  // --- Vertex buffer (a 3-vertex triangle). ---
  const Vertex verts[3] = {{-0.5f, -0.5f, 0.0f, 0.0f, 0.0f},
                           {0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
                           {0.0f, 0.5f, 0.0f, 0.5f, 1.0f}};
  const VkDeviceSize vbufSize = sizeof(verts);
  VkBuffer vbuf;
  VmaAllocation vbufAlloc;
  void* vbufMap = nullptr;
  if (!ctx->AllocateBuffer(&vbuf, &vbufAlloc, vbufSize, VMA_MEMORY_USAGE_CPU_TO_GPU,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &vbufMap, err)) {
    line(false, "vertex buffer alloc: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  std::memcpy(vbufMap, verts, vbufSize);
  {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(ctx->vma(), vbufAlloc, &info);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = info.deviceMemory;
    r.offset = 0;
    r.size = vbufSize;
    f.vkFlushMappedMemoryRanges(device, 1, &r);
  }

  // --- A 16x16 texture for the bindless array. ---
  VkImage tex;
  VmaAllocation texAlloc;
  VkImageCreateInfo tci = cbi;
  tci.extent = {16, 16, 1};
  tci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!ctx->AllocateImage(&tex, &texAlloc, tci, VMA_MEMORY_USAGE_GPU_ONLY, "pip-smoke-tex", err)) {
    line(false, "texture alloc: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  VkImageView texView;
  VkImageViewCreateInfo tvc = ivc;
  tvc.image = tex;
  f.vkCreateImageView(device, &tvc, nullptr, &texView);

  // --- Sampler (canonical, deduped). ---
  SamplerKey linear;  // defaults = linear filter, repeat.
  VkSampler sampler0 = descriptor.GetSampler(linear);
  const std::size_t samplersAfterFirst = descriptor.samplerCount();
  VkSampler samplerSame = descriptor.GetSampler(linear);  // should dedup (hit).
  const bool samplerDedup = (sampler0 == samplerSame) && (descriptor.samplerHits() >= 1);
  line(samplerDedup, "sampler dedup: identical config reuses one VkSampler (" +
                         std::to_string(descriptor.samplerCount()) + " unique, " +
                         std::to_string(descriptor.samplerHits()) + " hits)");
  (void)samplersAfterFirst;

  // --- Pipeline manager + variants. ---
  PipelineManager pm(ctx.get(), &descriptor);

  PipelineKey a;  // TRIANGLE_LIST, alpha blend, no depth write.
  a.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  a.alphaBlend = true;
  a.depthWrite = false;

  PipelineKey b = a;  // no blend (opaque).
  b.alphaBlend = false;

  PipelineKey c = a;  // TRIANGLE_FAN, depth write.
  c.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  c.depthWrite = true;

  const VkPipeline pa = pm.GetPipeline(a, passLayout.pass, vsSpirv, fsSpirv);
  const VkPipeline pb = pm.GetPipeline(b, passLayout.pass, vsSpirv, fsSpirv);
  const VkPipeline pc = pm.GetPipeline(c, passLayout.pass, vsSpirv, fsSpirv);
  line(pa && pb && pc, "3 pipeline variants created");
  const VkPipeline paAgain = pm.GetPipeline(a, passLayout.pass, vsSpirv, fsSpirv);
  line(paAgain == pa && pm.pipelineHits() >= 1,
       "pipeline cache hit: re-fetching variant A returns the same VkPipeline");
  line(pm.pipelineCount() == 3, "pipeline cache: 3 unique (got " +
                                    std::to_string(pm.pipelineCount()) + ")");

  // --- Descriptor set (from the per-frame pool) + cbuffer + texture + sampler. ---
  descriptor.BeginFrame();
  const VkDescriptorSet set = descriptor.AllocShaderSet();
  line(set != VK_NULL_HANDLE, "descriptor set allocated from the per-frame pool");

  struct CBuffer {
    float r, g, b, a;
  } cb{};
  cb = {1.0f, 0.25f, 0.25f, 1.0f};
  VkBuffer cbBuffer;
  VkDeviceSize cbOffset;
  const bool staged = descriptor.StageConstants(&cb, sizeof(cb), &cbBuffer, &cbOffset);
  line(staged, "constant buffer staged into the upload ring (offset " +
                   std::to_string(cbOffset) + ", ring used " +
                   std::to_string(descriptor.uploadRingUsed()) + " bytes)");
  if (set != VK_NULL_HANDLE) {
    descriptor.WriteUniform(set, ShaderBindings::kCbuffer, cbBuffer, cbOffset, sizeof(cb));
    descriptor.WriteTexture(set, 0, texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    descriptor.WriteSampler(set, 0, sampler0);
  }

  // --- Record a batch of draws (steady-state reuse). ---
  VkCommandPool pool;
  VkCommandBuffer cmdBuf;
  VkCommandPoolCreateInfo pcpi{};
  pcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pcpi.queueFamilyIndex = static_cast<uint32_t>(ctx->caps().graphicsFamily);
  f.vkCreateCommandPool(device, &pcpi, nullptr, &pool);
  VkCommandBufferAllocateInfo abci{};
  abci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  abci.commandPool = pool;
  abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  abci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(device, &abci, &cmdBuf);
  VkCommandBufferBeginInfo bbi{};
  bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(cmdBuf, &bbi);

  VkRenderPassBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = passLayout.pass;
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {W, H}};
  VkClearValue cc{0.1f, 0.1f, 0.1f, 1.0f};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &cc;
  f.vkCmdBeginRenderPass(cmdBuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);

  // Steady-state reuse: 9 draws cycling through the 3 cached pipelines. The
  // pipelines are created once (3) and rebound per draw (no re-creation).
  constexpr uint32_t kDraws = 9;
  const float blendConsts[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const VkPipeline* variants[3] = {&pa, &pb, &pc};
  for (uint32_t i = 0; i < kDraws; ++i) {
    const VkPipeline* pv = variants[i % 3];
    pm.BindAndDraw(cmdBuf, *pv, set, vbuf, 0, blendConsts, W, H, 3);
  }
  f.vkCmdEndRenderPass(cmdBuf);
  f.vkEndCommandBuffer(cmdBuf);

  VkFence fence;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(device, &fci, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmdBuf;
  const VkResult submitRes = f.vkQueueSubmit(ctx->graphicsQueue(), 1, &si, fence);
  const VkResult waitRes = f.vkWaitForFences(device, 1, &fence, VK_TRUE, 3000000000ull);
  line(submitRes == VK_SUCCESS && waitRes == VK_SUCCESS,
       "GPU executed " + std::to_string(kDraws) + " draws (submit=" +
           std::to_string(submitRes) + " wait=" + std::to_string(waitRes) + ")");
  const bool steadyStateOk =
      pm.pipelineCreates() == 3 && pm.bindPipelineCalls() == kDraws && pm.drawCalls() == kDraws;
  line(steadyStateOk,
       "steady-state reuse: " + std::to_string(pm.pipelineCreates()) +
           " pipeline creates, " + std::to_string(pm.bindPipelineCalls()) +
           " vkBindPipeline, " + std::to_string(pm.drawCalls()) + " draws");

  // --- Persistent (on-disk) pipeline cache: save + reload + recreate. ---
  std::string saveErr;
  const bool saved = ctx->SavePipelineCache(saveErr);
  // Reset the in-memory pipeline cache, reload from disk, and recreate the 3
  // variants: the VkPipelineCache now carries the compiled pipelines.
  pm.DestroyAll();
  std::string loadErr;
  ctx->LoadPipelineCache(loadErr);
  const VkPipeline pa2 = pm.GetPipeline(a, passLayout.pass, vsSpirv, fsSpirv);
  const VkPipeline pb2 = pm.GetPipeline(b, passLayout.pass, vsSpirv, fsSpirv);
  const VkPipeline pc2 = pm.GetPipeline(c, passLayout.pass, vsSpirv, fsSpirv);
  const bool cacheRoundTrip = saved && pa2 && pb2 && pc2 && pm.pipelineCount() == 3;
  line(cacheRoundTrip,
       "persistent pipeline cache: saved + reloaded, 3 pipelines recreated on top of the "
       "on-disk cache (blob " +
           std::to_string(ctx->PipelineCacheStats().diskCacheBytes) + " bytes)");

  // --- Teardown. ---
  f.vkDestroyFence(device, fence, nullptr);
  f.vkFreeCommandBuffers(device, pool, 1, &cmdBuf);
  f.vkDestroyCommandPool(device, pool, nullptr);
  pm.DestroyAll();
  f.vkDestroyImageView(device, texView, nullptr);
  f.vkDestroyImageView(device, rtView, nullptr);
  ctx->FreeImage(tex, texAlloc);
  ctx->FreeImage(rt, rtAlloc);
  ctx->FreeBuffer(vbuf, vbufAlloc);
  rpm.DestroyAll();
  descriptor.DestroyAll();
  line(true, "teardown complete");

  line(true, "pipelines: " + std::to_string(pm.pipelineCount()) +
                 " (created " + std::to_string(pm.pipelineCreates()) + ", hits " +
                 std::to_string(pm.pipelineHits()) +
                 "); samplers: " + std::to_string(descriptor.samplerCount()) + " unique");
  report += "\n" + ctx->memory().Describe() + "\n";
  std::printf("%s", report.c_str());
  if (!reportFile.empty()) {
    std::ofstream out(reportFile);
    out << report;
  }

  const bool pass = samplerDedup && staged && (submitRes == VK_SUCCESS) &&
                    (waitRes == VK_SUCCESS) && cacheRoundTrip && steadyStateOk;
  std::printf("\n=== fable2 pipeline/descriptor smoke test (W4): %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
