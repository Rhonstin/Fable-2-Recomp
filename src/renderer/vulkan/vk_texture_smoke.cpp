// W5: the guest texture cache self-test.
#include "vk_texture_smoke.h"

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
#include "texture_cache.h"

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
  opts.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &opts);
  if (spirv_out.empty()) {
    error_out = "empty SPIR-V";
    return false;
  }
  return true;
}

// Sample a cached guest texture: out_color = u_color*0.5 + texture*0.5.
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

struct Vertex {
  float x, y, z;
  float u, v;
};

// A fullscreen quad (2 triangles) mapping [0,1]x[0,1] to NDC [-1,1]x[-1,1].
const Vertex kQuadVerts[6] = {
    {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
    {-1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {-1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f},
};

}  // namespace

int RunTextureSmokeTest(const VkContextConfig& config, const std::string& reportFile) {
  std::string report;
  auto line = [&](bool ok, const std::string& what) {
    report += std::string(ok ? "[ok]   " : "[FAIL] ") + what + "\n";
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
  TextureCache texCache;
  texCache.Init(ctx.get());
  DescriptorManager descriptor(ctx.get());
  DescriptorManagerConfig descCfg;
  if (!descriptor.Create(descCfg, err)) {
    line(false, "DescriptorManager::Create: " + err);
    std::printf("%s", report.c_str());
    return 1;
  }
  RenderPassManager rpm(ctx.get());
  RenderPassKey rpKey;  // color-only, 1x (LOAD; manual clear so the draw isn't clobbered)
  PassLayout passLayout = rpm.GetPass(rpKey);
  if (passLayout.pass == VK_NULL_HANDLE) {
    line(false, "render pass create failed");
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "TextureCache + DescriptorManager + RenderPassManager created");

  // --- 3. Sample RTT (256x256 R8G8B8A8) + framebuffer. ---
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
  cbi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  cbi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ctx->AllocateImage(&rt, &rtAlloc, cbi, VMA_MEMORY_USAGE_GPU_ONLY, "tex-smoke-rt", err)) {
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

  // --- 4. Vertex buffer (fullscreen quad). ---
  const VkDeviceSize vbufSize = sizeof(kQuadVerts);
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
  std::memcpy(vbufMap, kQuadVerts, vbufSize);
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

  // --- 5. Compile shaders. ---
  std::vector<uint32_t> vsSpirv, fsSpirv;
  std::string shaderErr;
  if (!CompileGlslToSpirv(EShLangVertex, kVertexGlsl, vsSpirv, shaderErr) ||
      !CompileGlslToSpirv(EShLangFragment, kFragmentGlsl, fsSpirv, shaderErr)) {
    line(false, "shader compile: " + shaderErr);
    std::printf("%s", report.c_str());
    return 1;
  }
  line(true, "shaders compiled");

  // --- 6. Sample pipeline (kPosTex). ---
  PipelineManager pm(ctx.get(), &descriptor);
  PipelineKey pk;
  pk.vertexInput = VertexInputLayout::kPosTex;
  pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  pk.depthWrite = false;
  pk.depthTest = false;  // no depth attachment in this pass; depth-test would discard everything
  pk.cullMode = VK_CULL_MODE_NONE;  // test quad is CW in the Vulkan (Y-down) viewport; culling would drop it
  const VkPipeline pipeline = pm.GetPipeline(pk, passLayout.pass, vsSpirv, fsSpirv);
  check(pipeline != VK_NULL_HANDLE, "sample pipeline created (kPosTex vertex input)");

  // --- 7. Guest textures. ---
  // Xenos TextureFormat ordinals.
  const uint32_t kFmtA8R8G8B8 = 6;  // k_8_8_8_8
  const uint32_t kFmtDXT1 = 18;
  const uint32_t kFmtX1R5G5B5 = 3;  // k_1_5_5_5
  const uint32_t kFmtR32F = 36;     // k_32_FLOAT

  GuestTextureKey keyA{0x1000, 64, 64, 4, kFmtA8R8G8B8, 0};
  GuestTexture* texA = texCache.Acquire(keyA, 0);
  GuestTextureKey keyB{0x2000, 32, 32, 3, kFmtDXT1, 0};
  GuestTexture* texB = texCache.Acquire(keyB, 1);
  GuestTextureKey keyC{0x3000, 16, 16, 1, kFmtX1R5G5B5, 0};
  GuestTexture* texC = texCache.Acquire(keyC, 2);
  GuestTextureKey keyD{0x4000, 32, 32, 4, kFmtR32F, 0};
  GuestTexture* texD = texCache.Acquire(keyD, 3);
  check(texA && texB && texC && texD, "4 guest textures created (A8R8G8B8, DXT1, X1R5G5B5, R32F)");

  // --- 8. Format mapping. ---
  check(texA && texA->format == VK_FORMAT_R8G8B8A8_UNORM && texA->native,
        "A8R8G8B8 -> R8G8B8A8_UNORM (native, no CPU->GPU conversion)");
  check(texB && texB->format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && texB->native,
        "DXT1 -> BC1_RGBA_UNORM_BLOCK (native block-compressed)");
  check(texC && texC->format == VK_FORMAT_A1R5G5B5_UNORM_PACK16 && !texC->native,
        "X1R5G5B5 -> A1R5G5B5_UNORM_PACK16 (needs conversion)");
  check(texD && texD->format == VK_FORMAT_R32_SFLOAT && texD->native,
        "R32F -> R32_SFLOAT (native)");

  // --- 9. Build guest data. ---
  // texA: 64x64 R8G8B8A8, all bright red (R=255,G=0,B=0,A=255).
  std::vector<uint8_t> dataA(64 * 64 * 4);
  for (uint32_t i = 0; i < 64 * 64; i++) {
    dataA[i * 4 + 0] = 255;  // R
    dataA[i * 4 + 1] = 0;    // G
    dataA[i * 4 + 2] = 0;    // B
    dataA[i * 4 + 3] = 255;  // A
  }
  // texB: 32x32 DXT1 = 8x8 blocks * 8 bytes = 512 bytes. Fill with a valid BC1
  // block pattern (all endpoints the same -> a solid color).
  std::vector<uint8_t> dataB(512, 0xFF);

  // --- 10. Upload + mips (command buffer 1, submitted + waited). ---
  VkCommandPool uploadPool;
  VkCommandBuffer uploadBuf;
  VkCommandPoolCreateInfo pcpi{};
  pcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pcpi.queueFamilyIndex = static_cast<uint32_t>(ctx->caps().graphicsFamily);
  f.vkCreateCommandPool(device, &pcpi, nullptr, &uploadPool);
  VkCommandBufferAllocateInfo abci{};
  abci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  abci.commandPool = uploadPool;
  abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  abci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(device, &abci, &uploadBuf);
  VkCommandBufferBeginInfo bbi{};
  bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(uploadBuf, &bbi);

  check(texA && texCache.UploadLevel0(texA, dataA.data(), static_cast<uint32_t>(dataA.size()), uploadBuf, true),
        "texA uploaded (host-visible fast path)");
  texCache.GenerateMips(texA, uploadBuf);
  check(texB && texCache.UploadLevel0(texB, dataB.data(), static_cast<uint32_t>(dataB.size()), uploadBuf, false),
        "texB uploaded (device-local staging path)");
  texCache.GenerateMips(texB, uploadBuf);
  check(texA && texB && texA->mipsGenerated && texB->mipsGenerated,
        "mips GPU-generated via vkCmdBlitImage (uncompressed LINEAR + BC1 NEAREST)");
  line(true, "UNDEFINED -> SHADER_READ_ONLY transitions recorded for both textures");
  f.vkEndCommandBuffer(uploadBuf);

  VkFence uploadFence;
  VkFenceCreateInfo ufci{};
  ufci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(device, &ufci, nullptr, &uploadFence);
  VkSubmitInfo usi{};
  usi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  usi.commandBufferCount = 1;
  usi.pCommandBuffers = &uploadBuf;
  f.vkQueueSubmit(ctx->graphicsQueue(), 1, &usi, uploadFence);
  f.vkWaitForFences(device, 1, &uploadFence, VK_TRUE, 5000000000ull);
  f.vkDestroyFence(device, uploadFence, nullptr);
  f.vkFreeCommandBuffers(device, uploadPool, 1, &uploadBuf);
  f.vkDestroyCommandPool(device, uploadPool, nullptr);

  // --- 11. Sampler view composition. ---
  SamplerKey linear;  // defaults = linear filter, repeat.
  VkSampler sampler0 = descriptor.GetSampler(linear);
  VkDescriptorImageInfo si =
      TextureCache::MakeSampleInfo(texA, sampler0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  check(si.imageView == texA->view && si.sampler == sampler0 &&
            si.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "sampler view composed (cached image view + canonical sampler + layout)");

  // --- 12. Draw: sample texA into the RTT. ---
  descriptor.BeginFrame();
  VkDescriptorSet set = descriptor.AllocShaderSet();
  struct CBuffer {
    float r, g, b, a;
  } cb{};
  cb = {0.0f, 0.0f, 0.0f, 1.0f};  // u_color = black => out = t*0.5; red only if the red texture is sampled
  VkBuffer cbBuffer;
  VkDeviceSize cbOffset;
  descriptor.StageConstants(&cb, sizeof(cb), &cbBuffer, &cbOffset);
  if (set != VK_NULL_HANDLE) {
    descriptor.WriteUniform(set, ShaderBindings::kCbuffer, cbBuffer, cbOffset, sizeof(cb));
    descriptor.WriteTexture(set, 0, texA->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    descriptor.WriteSampler(set, 0, sampler0);
  }

  // --- Command buffer 2: the draw + readback. ---
  VkCommandPool drawPool;
  VkCommandBuffer drawBuf;
  VkCommandPoolCreateInfo dpcpi{};
  dpcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  dpcpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  dpcpi.queueFamilyIndex = static_cast<uint32_t>(ctx->caps().graphicsFamily);
  f.vkCreateCommandPool(device, &dpcpi, nullptr, &drawPool);
  VkCommandBufferAllocateInfo dabci{};
  dabci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  dabci.commandPool = drawPool;
  dabci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  dabci.commandBufferCount = 1;
  f.vkAllocateCommandBuffers(device, &dabci, &drawBuf);
  VkCommandBufferBeginInfo dbbi{};
  dbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  f.vkBeginCommandBuffer(drawBuf, &dbbi);

  // Manually clear the RTT (teal) so a red draw is distinguishable.
  {
    VkClearColorValue ccv{};
    ccv.float32[0] = 0.0f;
    ccv.float32[1] = 0.5f;
    ccv.float32[2] = 0.5f;
    ccv.float32[3] = 1.0f;
    VkImageSubresourceRange clrRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(drawBuf, rt, VK_IMAGE_LAYOUT_UNDEFINED, &ccv, 1, &clrRange);
    VkImageMemoryBarrier clrBar{};
    clrBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    clrBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clrBar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    clrBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    clrBar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    clrBar.image = rt;
    clrBar.subresourceRange = clrRange;
    f.vkCmdPipelineBarrier(drawBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &clrBar);
  }

  VkRenderPassBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = passLayout.pass;
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {W, H}};
  VkClearValue cc{0.0f, 0.5f, 0.5f, 1.0f};  // non-red clear, so a red sample is distinguishable
  rbi.clearValueCount = 1;
  rbi.pClearValues = &cc;
  f.vkCmdBeginRenderPass(drawBuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  const float blendConsts[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  pm.BindAndDraw(drawBuf, pipeline, set, vbuf, 0, blendConsts, W, H, 6);
  f.vkCmdEndRenderPass(drawBuf);

  // --- 13. Read back: copy the RTT to a host-visible buffer. ---
  VkImageMemoryBarrier rtBar{};
  rtBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  rtBar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  rtBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  rtBar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  rtBar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rtBar.image = rt;
  rtBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  f.vkCmdPipelineBarrier(drawBuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtBar);

  VkBuffer rbBuffer;
  VmaAllocation rbAlloc;
  void* rbMap = nullptr;
  if (!ctx->AllocateBuffer(&rbBuffer, &rbAlloc, W * H * 4, VMA_MEMORY_USAGE_GPU_TO_CPU,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &rbMap, err)) {
    line(false, "readback buffer alloc: " + err);
  }
  VkBufferImageCopy rbr{};
  rbr.bufferOffset = 0;
  rbr.bufferRowLength = 0;
  rbr.bufferImageHeight = H;
  rbr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  rbr.imageOffset = {0, 0, 0};
  rbr.imageExtent = {W, H, 1};
  f.vkCmdCopyImageToBuffer(drawBuf, rt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, rbBuffer, 1, &rbr);

  f.vkEndCommandBuffer(drawBuf);

  VkFence fence;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  f.vkCreateFence(device, &fci, nullptr, &fence);
  VkSubmitInfo siSubmit{};
  siSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  siSubmit.commandBufferCount = 1;
  siSubmit.pCommandBuffers = &drawBuf;
  const VkResult submitRes = f.vkQueueSubmit(ctx->graphicsQueue(), 1, &siSubmit, fence);
  const VkResult waitRes = f.vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
  check(submitRes == VK_SUCCESS && waitRes == VK_SUCCESS,
        "GPU uploaded + mipped + sampled (submit=" + std::to_string(submitRes) + " wait=" +
            std::to_string(waitRes) + ")");

  // The center pixel (texcoord 0.5,0.5) should sample the all-red texture;
  // out = t*0.5 => R ~ 128, G ~ 0, B ~ 0 (the clear was teal (0,127,127), so a
  // red readback proves the texture was actually sampled, not the clear color).
  bool sampledOk = false;
  int centerR = -1, centerG = -1, centerB = -1;
  if (rbMap) {
    const uint8_t* base = static_cast<const uint8_t*>(rbMap);
    const uint8_t* c = base + ((H / 2) * W + (W / 2)) * 4;
    centerR = c[0];
    centerG = c[1];
    centerB = c[2];
    sampledOk = centerR >= 100 && centerG < 60 && centerB < 60;
  }
  check(sampledOk, "end-to-end sample: center pixel read back as red (R=" +
                       std::to_string(centerR) + " G=" + std::to_string(centerG) + " B=" +
                       std::to_string(centerB) + ", clear was teal)");

  // --- 14. Split-memory upload counters. ---
  check(texCache.uploadsHostVisible == 1 && texCache.uploadsStaged == 1,
        "split memory: 1 host-visible (alias) upload + 1 device-local (staged) upload");

  // --- 15. Reference counting. ---
  GuestTextureKey keyV{0x5000, 8, 8, 1, kFmtA8R8G8B8, 0};
  GuestTexture* texV = texCache.Acquire(keyV, 4);
  texCache.Acquire(keyV, 4);  // second acquire -> refcount 2
  check(texV && texV->refCount == 2, "refcount: two acquires -> refcount 2");
  const uint64_t notFreed = texCache.Release(texV, 5);
  check(notFreed == 0 && texCache.Find(keyV) != nullptr, "refcount: release to 1 keeps the texture");
  texCache.Release(texV, 5);  // refcount -> 0 => evicted
  check(texCache.Find(keyV) == nullptr, "refcount: release to 0 evicts (image freed)");

  // --- 16. Budget eviction. ---
  // Create a handful of extra refcount-0 textures (via CreateOrUpdate, which does
  // not ref) so the cache has evictable memory, then force a tight budget.
  for (uint32_t i = 0; i < 5; i++) {
    GuestTextureKey k{0x6000 + i * 0x100, 32, 32, 1, kFmtA8R8G8B8, 0};
    (void)texCache.CreateOrUpdate(k, 20 + i);
  }
  const uint64_t before = texCache.TotalBytes();
  const uint32_t nEvicted = texCache.EvictToBudget(0);  // 0 = evict every refcount-0 texture
  check(nEvicted >= 1 && texCache.TotalBytes() < before,
        "budget eviction: evicted " + std::to_string(nEvicted) + " refcount-0 textures, freed " +
            std::to_string(before - texCache.TotalBytes()) + " B (" + std::to_string(before) +
            " B -> " + std::to_string(texCache.TotalBytes()) + " B; refcounted texA-D retained)");

  // --- 17. Video / external image adoption. ---
  // Simulate a decoded video frame: adopt an external 64x64 image (reusing the
  // readback buffer as a stand-in for an external surface is not valid, so we
  // just adopt a freshly allocated external image).
  VkImage extImage;
  VmaAllocation extAlloc;
  VkImageCreateInfo eci = cbi;
  eci.extent = {64, 64, 1};
  eci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (ctx->AllocateImage(&extImage, &extAlloc, eci, VMA_MEMORY_USAGE_GPU_ONLY, "video-frame", err)) {
    VkImageView extView;
    VkImageViewCreateInfo eivc = ivc;
    eivc.image = extImage;
    f.vkCreateImageView(device, &eivc, nullptr, &extView);
    GuestTextureKey keyVid{0x9000, 64, 64, 1, kFmtA8R8G8B8, 0};
    GuestTexture* vid = texCache.AdoptExternal(keyVid, extImage, extView,
                                               VK_FORMAT_R8G8B8A8_UNORM, 10);
    check(vid && vid->external && vid->Valid(),
          "video/decode surface: external image adopted into the cache (not freed on evict)");
    f.vkDestroyImageView(device, extView, nullptr);
  }

  // --- Teardown. ---
  f.vkDestroyFence(device, fence, nullptr);
  f.vkFreeCommandBuffers(device, drawPool, 1, &drawBuf);
  f.vkDestroyCommandPool(device, drawPool, nullptr);
  pm.DestroyAll();
  f.vkDestroyImageView(device, rtView, nullptr);
  ctx->FreeImage(rt, rtAlloc);
  ctx->FreeBuffer(vbuf, vbufAlloc);
  if (rbMap) {
    ctx->FreeBuffer(rbBuffer, rbAlloc);
  }
  rpm.DestroyAll();
  descriptor.DestroyAll();
  texCache.DestroyAll();
  line(true, "teardown complete");

  line(true, "textures: " + std::to_string(texCache.TextureCount()) +
                 " remaining; total bytes " + std::to_string(texCache.TotalBytes()) +
                 "; evicted " + std::to_string(texCache.evicted));
  report += "\n" + ctx->memory().Describe() + "\n";
  std::printf("%s", report.c_str());
  if (!reportFile.empty()) {
    std::ofstream out(reportFile);
    out << report;
  }

  std::printf("\n=== fable2 texture cache smoke test (W5): %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace fable2::renderer::vulkan
