# Custom Vulkan Renderer (rexgpu-fable2vulkan)

**STATUS: M0 VERIFIED — plugin loads and renders** — the plugin ABI,
graphics-system, command-processor seam, CMake target, build script, launch/
config selection and the `[gpu]` config key are all in place, and the
`rexgpu-fable2vulkan.dll` plugin **loads in-game and renders Fable 2 over
Vulkan** (Vulkan device on the host GPU + swapchain + presenting frames).
The game still ships with the stock `rexgpu-xenos` D3D12 plugin as the
default. This plan tracks the full effort of remaking the Fable 2 renderer as
a from-scratch, high-performance Vulkan backend that runs on all systems.

## Progress log

- Scaffold authored: `src/renderer/{plugin_main.cpp,
  fable2_vulkan_graphics_system.h/.cpp, fable2_vulkan_command_processor.h}`;
  CMake target `rexgpu-fable2vulkan` in `tools/vulkan_sdk/CMakeLists.txt` (the
  SDK-source build where the Vulkan deps are in scope); build script
  `tools/build_fable2_vulkan.cmd`; `--gpu_plugin=fable2-vulkan` handling in
  `src/core/fable_2_app.h`; `[gpu] renderer` key in `fable2_config.*` +
  `config/fable2_config.toml`.
- Strategy: the first increment subclasses the SDK's
  `VulkanGraphicsSystem` + `VulkanCommandProcessor` so the plugin renders
  correctly out of the box (M0/M1), then progressively replaces the costliest
  classes with custom implementations (the plan's reuse-then-rewrite approach).
- **M0 verified (2026):** `rexgpu-fable2vulkan.dll` built from the wrapper,
  exports `rex_gpu_abi_version` + `rex_gpu_create`, and — run as
  `fable_2.exe --gpu_plugin=fable2-vulkan` against the source-built Vulkan
  runtime — boots the game and renders it: `VulkanPresenter: Created 1920x1080
  swapchain`, `vsync present gate: suppressed N host presents`, Vulkan device
  on the RTX 3070. Note: it must run against the **source-built** Vulkan
  runtime (same as the stock `xenos-vulkan` flow) — the prebuilt D3D12 runtime
  lacks the Vulkan symbols the plugin imports.
- Known minor: this SDK build lacks the `depth_bias_shader` cvar, so the dog-fur
  depth-bias config logs a (benign) "cvars missing" warning. The hero/dog
  `readback_resolve_force_addresses` seeding likewise depends on that SDK
  cvar. Revisit if the SDK source regains those cvars.

## Why

The stock ReXGlue GPU plugin (`rexgpu-xenos.dll`) works but is a direct port of
Xenia's graphics system with Xenia's cost profile. The goal is a **custom
Vulkan renderer**:

- **Better performance / higher framerate** (the "Increased performance"
  checkbox in the README). Xenia-style per-packet CPU cost, frequent
  format-conversion, and cache churn are the main ceilings.
- **Works on all systems.** Vulkan is the single portable GPU API across
  Windows, Linux, (and with MoltenVK) macOS. One backend, one code path.
- **Full control over the pipeline** so we can optimize for Fable 2's actual
  workload instead of a general emulator's.

## What we are / are not rebuilding

The renderer is a **GPU plugin** loaded at launch via the SDK's GPU plugin ABI.
We are **not** re-emulating the Xenos chip from scratch — the SDK already
exposes a clean, backend-agnostic guest-side graphics pipeline (register file,
command-packet disassembler, base command processor, register-bank → SPIR-V
shader translator, Xenos format/ucode tables, shared memory, vsync worker). We
reuse that guest-side layer and build a **fresh, performance-focused Vulkan
host side** on top of it.

```
guest XEX (PPC, recompiled)
  └─ writes Xenos MMIO register file + command ring
        │
        ▼
  [SDK guest-side pipeline]  (reused)
    register_file · packet_disassembler · command_processor (base)
    shader translator (Xenos RB → SPIR-V) · format/ucode tables · shared_memory
        │   emits backend-agnostic draw/copy/blit/resolve ops
        ▼
  [FABLE 2 VULKAN CORE]  (NEW — this project)
    VkContext · memory (VMA) · render passes · descriptor sets
    pipeline state · command recording · image/view/sampler management
    texture/RT/shared-memory caching · swapchain present · sync
        │
        ▼
  [SDK ui::vulkan provider]  (reused) — instance/device/swapchain/VMA/UI
```

Reused vs. new boundary (mirrors how the SDK's own Vulkan backend is layered):

- **Reuse (SDK, `rex::graphics` + `rex::ui::vulkan`):** `GraphicsSystem` base,
  `register_file`, `packet_disassembler`, `CommandProcessor` base class,
  SPIR-V shader translator + `Shader`/`PipelineCache` plumbing, `format/ucode`
  tables, `sampler_info`, `shared_memory` (base), vsync worker,
  `ui::vulkan::VulkanProvider` (device/instance/swapchain/VMA/UI/ImGui).
- **New (this project):** the Vulkan command processor and all host-side
  caches/management it drives — texture cache, render-target cache, pipeline
  cache, shared-memory manager, primitive processor, plus our own performance
  optimizations. See workstreams below.

## Plugin ABI & integration

The plugin is a shared library named `rexgpu-<name>` staged next to the exe,
loaded by `rex::system::LoadGpuPlugin(name, backend)`. It exports:

- `uint32_t rex_gpu_abi_version(void)` → `kGpuPluginAbiVersion` (currently 1)
- `rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi, const GpuCreateInfo*)`

The app selects the renderer at launch with `--gpu_plugin=<name>` (see
`src/core/fable_2_app.h`); the stock path already does this for
`--gpu_plugin=xenos-vulkan`. We add `--gpu_plugin=fable2-vulkan`.

- [x] Create plugin target `rexgpu-fable2vulkan` (SHARED) in the project
      CMake; links `rexruntime` (PRIVATE), `Vulkan::Headers`,
      `GPUOpen::VulkanMemoryAllocator`, `glslang::SPIRV`, `xxHash::xxhash`
      (defined in `tools/vulkan_sdk/CMakeLists.txt`, the SDK-source build where
      the Vulkan deps are in scope).
- [x] Export `rex_gpu_abi_version()` returning `kGpuPluginAbiVersion`.
- [x] Export `rex_gpu_create()` that returns `new Fable2VulkanGraphicsSystem()`.
- [x] `Fable2VulkanGraphicsSystem : rex::graphics::vulkan::VulkanGraphicsSystem`
      - [x] `name()` → `"fable2-vulkan"`.
      - [x] `CreateProvider(with_presentation)` — inherited from the SDK base
            (`ui::vulkan::VulkanProvider::Create(true, with_presentation)`).
      - [x] `CreateCommandProcessor()` → `Fable2VulkanCommandProcessor`.
- [x] Plugin compiles with `-Wno-unused-variable` (no PCH; the SDK headers are
      self-contained).
- [x] Stage `rexgpu-fable2vulkan.dll` next to the exe (`tools/build_fable2_vulkan
      .cmd` + manual copy; Release verified — Debug variant to be confirmed).
- [x] `fable_2_app.h::OnPreSetup`: handle `--gpu_plugin=fable2-vulkan` by
      calling `LoadGpuPlugin("fable2vulkan", "vulkan")` (falls back to stock if
      the plugin is not staged).
- [x] Config toggle `[gpu] renderer = "stock" | "fable2"` in `fable2_config.*`
      + `config/fable2_config.toml`; D3D12 stock stays the default.
- [x] A/B runnable without rebuild: stock and fable2 plugins coexist next to
      the exe; pick with `--gpu_plugin` or config.

## Workstream 1 — Vulkan foundation (VkContext)

A clean, self-contained Vulkan context that both the UI provider path and our
command processor can rely on. (The SDK's `ui::vulkan` provider already gives
us instance/device/swapchain/VMA — reuse it where it fits, but keep a thin
abstraction so we can swap in custom device/queue/memory decisions.)

- [ ] `VkContext`: wrap `VkInstance`, `VkPhysicalDevice`, `VkDevice`, queue
      families (graphics, transfer, compute) and queue handles.
- [ ] Device feature set: expose what the guest needs (depth clamping,
      separate storage images, wide lines off, timestamp compute and graphics,
      sampler Anisotropy, `samplerFilterMinmax` for FXAA, 16-bit/float formats,
      `robustBufferAccess`, `independentBlend`, 4444/565 format support).
- [ ] Device extensions: request the minimum set that keeps us portable
      (baseline 1.0) and opportunistically enable 1.1/1.2 + KHR extensions that
      improve perf (`timeline semaphore`, `buffer device address`,
      `subgroup`, `8bit`, `16bit storage`, `extended dynamic state`,
      `sampler YcbCr`/`sampler filter minmax`).
- [ ] Physical-device limits probe (max viewports, image array layers,
      max draw count, max image dimensions/arrays, buffer/image granularity).
- [ ] Queue selection + optional adaptive-queue for hybrid GPU laptops (prefer
      discrete; avoid the iGPU TDR path noted in `src/main.cpp`).
- [ ] Memory: integrate VMA; define memory-type selection helpers
      (device-local, host-visible, host-coherent, imported).
- [ ] Validation layers: optional, on in Debug, controllable by config;
      custom validation callbacks for our invariants (no per-draw allocation,
      no missing barrier before present).
- [ ] Error handling: every `vk*` call checked; descriptive log on failure;
      GPU-loss / device-reset path wired to the UI provider's
      `host_gpu_loss_callback`.

## Workstream 2 — Frame sync & presentation

- [ ] Frame synchronizer: N in-flight frames (default 2–3); per-frame
      `VkFence`/`VkSemaphore` ring; `vkAcquireNextImage` with timeout +
      timeout fallback (skip present rather than block).
- [ ] Swapchain: sized to the window, `VK_PRESENT_MODE_FIFO` (vsync on) /
      `VK_PRESENT_MODE_MAILBOX` or `IMMEDIATE` (vsync off / unlimited);
      recreate on resize with graceful frame-dropping.
- [ ] Vblank pacing: match the guest vblank cadence from the SDK vsync worker
      (`guest_vblank_pacing` cvar); decouple guest frame production from host
      present when uncapped.
- [ ] Present the final composite to the swapchain image; handle
      `VK_SUBOPTIMAL` (recreate) and `VK_ERROR_OUT_OF_DATE_KHR`.
- [ ] Overlay/UI compositing: draw the SDK UI/ImGui pass into the same frame
      after the game render (reuse `ui::vulkan` presenter ordering).
- [ ] Render-target readback path: implement the `readback_resolve_force_addresses`
      mechanism (hero/dog texture fix) in the new renderer so the existing
      config keeps working.

## Workstream 3 — Render passes & subpass design

- [ ] Model the guest frame as: guest scene render into guest RTs → optional
      GPU-side downscale/resolve → final composite to swapchain.
- [ ] Primary render pass over the swapchain image (color + optional MSAA
      resolve subpass).
- [ ] Guest-side render passes: color-only, color+depth, with/without MSAA,
      with/without depth writes — a small fixed set of cached render passes
      keyed by (attachments, MSAA, blend, sample count, framebuffer fetch).
- [ ] Secondary render passes for render-to-texture (RTT) / off-screen guest
      targets; blit/copy/resolve subpass or standalone compute.
- [ ] Depth stencil: single-stencil, single-attachment, and MSAA-attachment
      variants; correct layout transitions.
- [ ] Framebuffer-fetch (Xenos `FETCH` / memory-export) path: emulate with
      dual-source blend / feedback loops (subpass input attachments) where
      possible, fallback to read-and-reissue where not.

## Workstream 4 — Descriptor sets & pipeline state (the hot path)

This is where most of the perf win lives. Xenia re-creates descriptor sets and
pipeline state per draw with high overhead; we want steady-state pipeline
reuse.

- [ ] **Pipeline cache (persistent):** key pipelines on (vertex input layout,
      rasterizer, depth/stencil, blend, sample count, shader stage hashes,
      dynamic-state subset, viewport count). On-disk cache keyed by
      title-id + renderer version (reuse the SDK `InitializeShaderStorage`
      hook). Target: after first load, near-100% pipeline cache hit rate.
- [ ] **Descriptor set pool / arena:** preallocated, per-frame-reset descriptor
      set allocator (reuse the SDK `linked_type_descriptor_set_allocator` /
      `single_layout_descriptor_set_pool` patterns). Zero allocations per draw.
- [ ] **Descriptor layout cache:** small number of stable layouts (vertex
      index buffer, constant/UBO, samplers+textures arrays). Avoid
      per-pipeline layouts where possible; prefer a shared sampler layout +
      bound images.
- [ ] **Dynamic state:** enable the full dynamic-state set (viewport, scissor,
      blend constants, depth bounds, line width, rasterizer offsets) to reduce
      pipeline variants. Optionally use VK_EXT_extended_dynamic_state if
      present.
- [ ] **Bindless / indirect texture & RT binding:** index-based texture/RT
      arrays so a single descriptor set can reference any cached image — the
      key to avoiding per-texture descriptor churn (big for Fable 2's
      streaming textures).
- [ ] Constant buffer (guest `cbuffer`) staging: one upload buffer per frame
      for all immediate constant data; `VK_BUFFER_USAGE_UNIFORM_BUFFER`
      mapped ring. Minimize `vkUpdateBuffer` per-draw.
- [ ] UBO/SSBO/ROBO (Xenos register-bank constant sources): map the guest
      register banks into coherent storage/uniform buffers; batch updates.
- [ ] Reduce `vkBindDescriptorSets` / `vkBindPipeline` frequency: coalesce
      consecutive draws that share pipeline state; reorder or group where the
      guest allows (careful: preserve draw order for correctness).
- [ ] Sampler cache: dedup identical sampler states; a handful of canonical
      samplers + dynamic for the rest.

## Workstream 5 — Texture cache (guest image manager)

- [ ] Texture cache keyed by (guest address, size, format, mip level,
      protection). Reference-counted; eviction to free image memory.
- [ ] Format handling: map Xenos formats → Vulkan. Keep uncompressed formats
      (A8R8G8B8, X1R5G5B5, A1R5G5B5, G16R16, R11G11B10, etc.) in native
      layout; convert only where Vulkan can't represent the format natively
      (BC1/BC3/BC5/BC6H/BC7 need BCn support; R32/R32G32 float; etc.).
- [ ] **Minimize CPU→GPU format conversion:** for streaming textures, detect
      already-Vulkan-native formats and map guest memory as device-local
      (import/alias) instead of uploading+converting every frame. This is a
      major Fable 2 perf lever (large streaming textures).
- [ ] Mip generation: GPU `vkCmdBlitImage` mips, cached; on-demand per level.
- [ ] Image layout transitions + aliasing: allow textures to alias freed
      memory; correct `UNDEFINED → SHADER_READ_ONLY` transitions.
- [ ] Sampler view creation with the cached sampler; anisotropy from the
      launcher setting.
- [ ] Handle the "split memory" model (guest memory not always host-mapped):
      correct copy vs. alias decision based on whether the source is
      host-visible.
- [ ] Video / decode surfaces: accept decoded video frames (bik) as external
      images; texture-cache them like any other texture.

## Workstream 6 — Render-target cache (guest framebuffer manager)

- [ ] RT cache keyed by (guest address, extent, format, msaa, color/depth
      usage). Reference-counted with LRU-ish eviction.
- [ ] MSAA: 1x/2x/4x multisampled color + depth, resolve to the single-sample
      guest target (the game's native MSAA path).
- [ ] Depth formats: map to Vulkan depth formats (X24S8, G16R16, R32 uint
      depth, float depth); support `depthCompareOp`/`stencilOp` from blend
      state.
- [ ] Preserve guest RT contents across frame (do not clear unless the guest
      clears); correct `ATTACHMENT_OPTIMAL` layout on first touch.
- [ ] Framebuffer objects keyed by (RT set, render pass); reuse rather than
      recreate.
- [ ] Render-to-texture + blit/resolve to guest memory (the scratch texture at
      `0x12704000` used by the hero/dog morph path).
- [ ] Readback (CPU) of guest memory when the CPU locks a texture; coalesce
      and async via a dedicated readback queue/staging buffer.

## Workstream 7 — Shared memory & blits/copy

- [ ] Guest shared memory (Xenos `SHARED` region) manager: map guest shared
      memory to a coherent `VkBuffer`; handle `COPY_TO_SHMEM`, `COPY_FROM_SHMEM`,
      `SHARED_FILL`, `SHARED_SWAP` packets.
- [ ] Immediate copy/blit commands: 2D blit, scale, fill (rect/clear), and
      16bpp/32bpp resolve blits the guest issues directly.
- [ ] Fill/clear: `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage` /
      `vkCmdClearAttachments` where possible, `vkCmdFillBuffer` for buffer
      fills.
- [ ] Correct barrier between shared-memory copy and subsequent texture use.

## Workstream 8 — Primitives, vertex/index buffers & draw dispatch

- [ ] Vertex/index buffers: guest memory → `VkBuffer` (import/alias if
      host-visible, else upload via a per-frame upload ring). Handle 16-bit
      index wrap and 32-bit indices.
- [ ] Primitive topology mapping (point/line/triangle, with/without adjacency,
      strips, lists); clamp/point/line rasterizer modes.
- [ ] Draw calls: `vkCmdDraw` / `vkCmdDrawIndexed`; indirect for
      GPU-instanced / geometry-shader-emitted draws.
- [ ] Geometry/patch shader emulation: Xenos has no GS/DS — the SDK
      pre-translates the fixed-function topology expanders (quad/triangle
      tessellation, adaptive tessellation) into SPIR-V. Wire those translated
      shaders (HS/VS equivalents) into the pipeline; ensure the
      adaptive-tessellation path is present (see `src/graphics/shaders/`).
- [ ] Tessellation factors and culling from the guest rasterizer state.
- [ ] Viewport/scissor (5 viewports on Xenos; support multi-viewport draws).

## Workstream 9 — Shader translation & pipeline assembly

Reuse the SDK's register-bank → SPIR-V translator (it already emits valid
SPIR-V including fetch/memory-export/register-bank stages). We consume its
output; we own pipeline assembly.

- [ ] Compile translated SPIR-V into `VkShaderModule` (caching by hash);
      validate with SPIRV-Tools in Debug.
- [ ] Assemble `VkPipeline` per stage (VS, HS, DS, GS, FS) from the
      translator's output + the guest blend/raster/viewport state.
- [ ] Specialization / dynamic uniform buffers for per-draw constant blocks.
- [ ] Handle the shader "interpreter" fallback path (some guests ship
      uncompiled bytecode interpreted on the host) — decide whether to
      keep it or require translation; log when hit.
- [ ] Pipeline layout compatibility: share descriptor set layouts across
      pipelines to keep state-change cost low (see WS4).

## Workstream 10 — Performance program (Fable 2 specific)

- [ ] Baseline: build a profiler hook that reports (a) guest ms/frame,
      (b) CPU command-encoding time per frame, (c) draw/descriptor/pipeline
      call counts, (d) texture/RT cache hit rate, (e) format-conversion bytes.
- [ ] Target: reach/beat the stock D3D12 plugin's framerate at the same
      resolution + uncapped; document the delta per workstream.
- [ ] Cut per-packet CPU cost: batched ring consumption, avoid
      per-packet heap allocation, `memcpy`-only fast paths for clear/fill.
- [ ] Multi-threaded command encoding (if the guest frame maps to independent
      render targets) — record guest RT passes on worker threads, present on
      the main thread; keep the vsync worker off the critical path.
- [ ] Avoid per-frame texture upload storms (native-format aliasing, WS5).
- [ ] Avoid MSAA resolve cost where the guest uses 1x (detect and skip).
- [ ] Post-FX passes (FXAA, gamma) — implement as lightweight fullscreen
      compute/PS; reuse the SDK's compiled post-FX shaders where they fit.
- [ ] Framerate/fps-cap interplay: verify the vsync worker + our present
      path respect the 30/60/120/144/165/240/unlimited FPS options.

## Workstream 11 — Cross-system robustness

- [ ] Build on Windows (clang + lld + Ninja, matching the stock plugin build)
      and on Linux (GCC/Clang). The plugin is pure C++ + Vulkan; no D3D.
- [ ] Dynamic Vulkan loader (load `vulkan-1.dll` / `libvulkan.so.1`); no
      import library required (matches `src/vulkan_smoke.cpp`).
- [ ] Feature-detect and degrade: minimum-viable on a Vulkan 1.0 core device
      (older iGPU / integrated); enhanced path on 1.1/1.2. Log the active
      tier at startup.
- [ ] Adapter enumeration + selection: pick best device for the requested
      work; prefer discrete on hybrid laptops; allow override by config.
- [ ] macOS via MoltenVK (the SDK's `vulkan_moltenvk` path) — smoke-test only,
      not a primary target.
- [ ] Graceful failure: if no suitable device, log + fall back to the stock
      plugin (do not hard-crash the game).
- [ ] Thread-safety of the plugin's global state; clean shutdown
      (`Shutdown()` tears down device before the loader handle closes).

## Workstream 12 — Correctness & validation vs. reference

- [ ] Side-by-side screenshot diff harness: run stock D3D12 and fable2-vulkan
      on the same save + same inputs; capture frames; diff (per-pixel and
      perceptual) to catch color/depth/blend/MSAA regressions.
- [ ] Cover Fable 2's full feature set with targeted scenes:
      - [ ] Menu / HUD (GUI pass, immediate-mode UI, text).
      - [ ] Outdoors day/night, MSAA on/off, high + low detail.
      - [ ] Water / reflective surfaces (render-to-texture + reflection).
      - [ ] Indoors with dynamic shadows + lightmaps.
      - [ ] Hero/dog skin morphs (realtime texture morphing path).
      - [ ] Particle effects / spell VFX (heavy overdraw, blending).
      - [ ] Cutscenes + intro/credits video (bik decode).
      - [ ] Post-process (FXAA, gamma, motion blur disabled/enabled).
- [ ] Stress: uncapped FPS for 10 min in a dense scene; no TDR, no OOM, no
      growing GPU memory (cache eviction working).
- [ ] Memory: track peak GPU memory; keep under the stock plugin's high-water
      mark (or document why).

## Workstream 13 — Testing & CI

- [ ] Unit tests for the new caches (texture/RT/shared-memory) — eviction,
      aliasing, layout transition correctness (a headless device or
      software/llvmpipe driver).
- [ ] A headless/`VK_LAYER_KHRONOS`-free path: run the command processor with
      a `null` / headless provider to unit-test packet → command translation
      without a display.
- [ ] Reuse the existing `src/vulkan_smoke.cpp` pattern for a minimal
      standalone device-creation test target.
- [ ] Wire a `tools/` script to build + run the fable2 plugin against a fixed
      save and capture a golden-frame set; add to a local "perf & correctness"
      check.

## Milestones (definition of done per stage)

- [x] **M0 — Loadable plugin.** `rexgpu-fable2vulkan.dll` builds, exports the
      ABI, loads via `--gpu_plugin=fable2-vulkan`, and `SetupPresentation`
      brings up a window via the SDK Vulkan provider. *(verified: 1920x1080
      swapchain + presenting frames over Vulkan on the host GPU.)*
- [ ] **M1 — Guest boots to the main menu.** The guest renders the Fable 2
      menu with a working command processor (baseline reuse of SDK guest
      pipeline) and present. Menu is navigable.
- [ ] **M2 — Playable world, correct.** An outdoor + indoor scene renders
      correctly (color, depth, MSAA, lighting) and matches the stock plugin
      within tolerance in the screenshot harness.
- [ ] **M3 — Feature-complete.** RTT, water, morphs, particles, video, post-FX
      all work; a full game session (menu → play → cutscene → back) is
      stable.
- [ ] **M4 — Performance target met.** Framerate ≥ stock D3D12 at the same
      settings (uncapped + 60 + 144), verified with the profiler; texture
      upload + descriptor churn reduced measurably.
- [ ] **M5 — Cross-system + default.** Builds and runs on Windows and Linux;
      graceful degradation on low-tier Vulkan; option to make it the default
      renderer (config), with stock D3D12 as fallback.

## Open questions / risks

- **Reuse vs. rewrite depth:** the SDK's `CommandProcessor` base + caches are
  large (~21k lines of Vulkan in the SDK alone). Decide per-class whether to
  (a) subclass the SDK Vulkan classes and override the hot methods, or (b)
  reimplement standalone for full control. Start with (a) to hit M1/M2 fast,
  then replace the costliest classes with (b) for M4.
- **Split-memory aliasing:** whether the guest texture memory is
  host-visible determines alias-vs-upload; confirm the memory model the
  ReXGlue runtime uses for the graphics heap before relying on aliasing.
- **Frame buffer fetch / memory export fidelity:** the register-bank
  memory-export translator covers the common cases; verify Fable 2 doesn't
  hit an unsupported fetch pattern.
- **Geometry-shader emulation cost:** the tessellation/topology-expander
  shaders add vertex count; confirm they don't become a bottleneck in
  dense-geometry scenes.
- **ABI version:** our plugin must stay in lockstep with `kGpuPluginAbiVersion`
  (currently 1); any interface change in the SDK bumps it and we follow.

## Files / layout (proposed)

```
src/renderer/
  plugin_main.cpp                 # ABI exports
  fable2_vulkan_graphics_system.h/.cpp   # IGraphicsSystem impl (thin)
  vulkan/
    vk_context.h/.cpp             # instance/device/queues/VMA
    frame_sync.h/.cpp             # swapchain + fences/semaphores
    render_pass.h/.cpp            # render pass + framebuffer cache
    descriptor_manager.h/.cpp     # set pools, layout cache, bindless arrays
    pipeline_manager.h/.cpp       # pipeline cache (in-mem + on-disk)
    texture_cache.h/.cpp          # guest image manager
    render_target_cache.h/.cpp    # guest framebuffer manager
    shared_memory.h/.cpp          # guest SHARED region manager
    primitive_processor.h/.cpp    # topology/draw dispatch
    command_processor.h/.cpp      # guest packet -> command encoding (hot path)
    shader.h/.cpp                 # SPIR-V -> VkShaderModule + assembly
    postfx.h/.cpp                 # FXAA/gamma fullscreen passes
  CMakeLists.txt                  # rexgpu-fable2vulkan target
```

Config (in `fable2_config.toml`), new `[gpu]` section:
```toml
[gpu]
renderer = "stock"     # "stock" (rexgpu-xenos D3D12) | "fable2" (rexgpu-fable2vulkan)
```
Overridable at launch with `--gpu_plugin=fable2-vulkan`.
