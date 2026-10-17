// W10 — Post-FX passes implementation.
#include "post_fx.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>
#include "vk_function_table.h"

// GLSL for post-FX passes.
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

const char* kFullscreenVertexGlsl = R"(
#version 450
layout(location=1) out vec2 v_uv;
void main() {
    int i = gl_VertexIndex;
    vec2 pos;
    vec2 uv;
    if (i == 0) { pos = vec2(-1,-1); uv = vec2(0,1); }
    else if (i == 1) { pos = vec2(1,-1); uv = vec2(1,1); }
    else if (i == 2) { pos = vec2(-1,1); uv = vec2(0,0); }
    else { pos = vec2(1,1); uv = vec2(1,0); }
    gl_Position = vec4(pos, 0, 1);
    v_uv = uv;
}
)";

const char* kGammaFragGlsl = R"(
#version 450
layout(location=1) in vec2 v_uv;
layout(location=0) out vec4 fragColor;
layout(set=0, binding=1) uniform texture2D inputImage;
layout(set=0, binding=2) uniform sampler inputSampler;
const float gamma = 2.2;
void main() {
    vec4 c = texture(sampler2D(inputImage, inputSampler), v_uv);
    fragColor = vec4(pow(c.rgb, vec3(1.0/gamma)), c.a);
}
)";

const char* kFXAAFragGlsl = R"(
#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(location=1) in vec2 v_uv;
layout(location=0) out vec4 fragColor;
layout(set=0, binding=1) uniform texture2D inputImage;
layout(set=0, binding=2) uniform sampler inputSampler;
float luma(vec4 c) { return dot(c.rgb, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 res = vec2(textureSize(inputImage, 0));
    float lM = luma(texture(sampler2D(inputImage, inputSampler), v_uv));
    float lN = luma(texture(sampler2D(inputImage, inputSampler), v_uv + vec2(0, 1)/res));
    float lS = luma(texture(sampler2D(inputImage, inputSampler), v_uv + vec2(0, -1)/res));
    float lE = luma(texture(sampler2D(inputImage, inputSampler), v_uv + vec2(1, 0)/res));
    float lW = luma(texture(sampler2D(inputImage, inputSampler), v_uv + vec2(-1, 0)/res));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float lRange = lMax - lMin;
    if (lRange < max(0.03125, 0.125 * lMax)) {
        fragColor = texture(sampler2D(inputImage, inputSampler), v_uv);
        return;
    }
    float dx = (lN + lE) - (lS + lW);
    float dy = (lN + lW) - (lS + lE);
    vec2 dir = abs(dx) > abs(dy) ? vec2(1.0/abs(dx), 0) : vec2(0, 1.0/abs(dy));
    vec2 uv1 = v_uv - dir / res;
    vec2 uv2 = v_uv + dir / res;
    vec4 c1 = texture(sampler2D(inputImage, inputSampler), uv1);
    vec4 c2 = texture(sampler2D(inputImage, inputSampler), uv2);
    vec4 avg = (c1 + c2) * 0.25;
    float lAvg = luma(avg);
    if ((lAvg < lMin || lAvg > lMax) && (lAvg < lM != lM < lMin)) {
        fragColor = avg;
        return;
    }
    fragColor = texture(sampler2D(inputImage, inputSampler), v_uv);
}
)";

// Compile GLSL to SPIR-V.
bool CompileGlsl(const char* glsl, EShLanguage stage, std::vector<uint32_t>& outSpirv, std::string* err = nullptr) {
  outSpirv.clear();
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  glslang::TShader shader(stage);
  const char* src = glsl;
  shader.setStrings(&src, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&DefaultResource(), 450, false, messages)) {
    if (err) *err = "GLSL parse: " + std::string(shader.getInfoLog());
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    return false;
  }
  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (!intermediate) return false;
  glslang::GlslangToSpv(*intermediate, outSpirv);
  return true;
}

}  // namespace

namespace fable2::renderer::vulkan {

PostFX::~PostFX() { Shutdown(); }

bool PostFX::Init(VkContext* ctx, DescriptorManager* descriptors, PipelineManager* pipelines,
                  RenderPassManager* renderPasses, std::string& err) {
  ctx_ = ctx;
  descriptors_ = descriptors;
  pipelines_ = pipelines;
  renderPasses_ = renderPasses;

  // Compile shaders.
  std::vector<uint32_t> vsSpirv, gammaSpirv, fxaaSpirv;
  std::string shaderErr;
  if (!CompileGlsl(kFullscreenVertexGlsl, EShLangVertex, vsSpirv, &shaderErr)) {
    err = "PostFX: vertex shader compile failed: " + shaderErr;
    return false;
  }
  if (!CompileGlsl(kGammaFragGlsl, EShLangFragment, gammaSpirv, &shaderErr)) {
    err = "PostFX: gamma shader compile failed: " + shaderErr;
    return false;
  }
  if (!CompileGlsl(kFXAAFragGlsl, EShLangFragment, fxaaSpirv, &shaderErr)) {
    err = "PostFX: FXAA shader compile failed: " + shaderErr;
    return false;
  }

  // Create render pass (color-only, CLEAR).
  RenderPassKey rpKey;
  rpKey.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  rpKey.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  gammaPass_ = renderPasses_->GetPass(rpKey).pass;
  fxaaPass_ = renderPasses_->GetPass(rpKey).pass;
  if (gammaPass_ == VK_NULL_HANDLE || fxaaPass_ == VK_NULL_HANDLE) {
    err = "PostFX: render pass creation failed";
    return false;
  }

  // Create pipelines.
  PipelineKey pk;
  pk.vertexInput = VertexInputLayout::kNone;  // Fullscreen quad (uses gl_VertexIndex).
  pk.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  pk.cullMode = VK_CULL_MODE_NONE;
  pk.depthTest = false;
  pk.depthWrite = false;

  gammaPipeline_ = pipelines_->GetPipeline(pk, gammaPass_, vsSpirv, gammaSpirv);
  fxaaPipeline_ = pipelines_->GetPipeline(pk, fxaaPass_, vsSpirv, fxaaSpirv);
  if (gammaPipeline_ == VK_NULL_HANDLE || fxaaPipeline_ == VK_NULL_HANDLE) {
    err = "PostFX: pipeline creation failed";
    return false;
  }

  initialized_ = true;
  return true;
}

void PostFX::Shutdown() {
  if (ctx_) {
    if (quadAlloc_) {
      ctx_->FreeBuffer(quadBuffer_, quadAlloc_);
    }
    gammaPipeline_ = VK_NULL_HANDLE;
    fxaaPipeline_ = VK_NULL_HANDLE;
    gammaPass_ = VK_NULL_HANDLE;
    fxaaPass_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
  }
}

void PostFX::ApplyGamma(VkCommandBuffer cmd, VkImage inputImage, VkImage outputImage,
                        uint32_t width, uint32_t height) {
  if (!initialized_ || !ctx_) return;
  Functions& f = ctx_->functions();

  // Bind pipeline.
  f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gammaPipeline_);

  // Bind descriptor set (input image + sampler).
  VkDescriptorSet dset = descriptors_->AllocShaderSet();
  // The caller must have already written the input image to binding 1
  // and the sampler to binding 2. For now, we just bind the set.
  f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            descriptors_->PipelineLayout(), 0, 1, &dset, 0, nullptr);

  // Draw fullscreen quad.
  f.vkCmdDraw(cmd, 4, 1, 0, 0);
  gammaPasses++;
}

void PostFX::ApplyFXAA(VkCommandBuffer cmd, VkImage inputImage, VkImage outputImage,
                       uint32_t width, uint32_t height) {
  if (!initialized_ || !ctx_) return;
  Functions& f = ctx_->functions();

  f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaaPipeline_);

  VkDescriptorSet dset = descriptors_->AllocShaderSet();
  f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            descriptors_->PipelineLayout(), 0, 1, &dset, 0, nullptr);

  f.vkCmdDraw(cmd, 4, 1, 0, 0);
  fxaaPasses++;
}

}  // namespace fable2::renderer::vulkan
