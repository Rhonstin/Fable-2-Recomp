# Wrapper project to build the ReXGlue SDK's Vulkan GPU plugin.
#
# Why a wrapper? Building the SDK directly as a top-level project runs its
# install/export step (cmake/rexglue_install.cmake). For the Vulkan config that
# step appends the glslang targets to its install list by their *raw* names
# (e.g. "SPIRV"), but those targets only exist namespaced (glslang::SPIRV), so
# configure fails with:  install TARGETS given target "SPIRV" which does not
# exist.  (The SDK's top-level Vulkan-on-Windows install path is incomplete.)
#
# Consuming the SDK via add_subdirectory() makes PROJECT_IS_TOP_LEVEL false,
# which skips that install step entirely. We only need the built
# rexgpu-xenos.dll (Vulkan), so skipping install/packaging is fine. The
# wrapper modifies nothing - the SDK tree it consumes is stock upstream plus
# this project's tracked local patch (thirdparty/rexglue-sdk-local.patch).
cmake_minimum_required(VERSION 3.25)

# C/CXX enabled so the SDK's C and C++ dependencies configure. The compiler
# itself is selected via -DCMAKE_C(CXX)_COMPILER on the command line, which
# must precede this project() call.
project(rexglue_vulkan_plugin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Release)
endif()

# Both backends on. The resulting rexruntime.dll is a SUPERSET that exports both
# the D3D12 symbols (so the game + the D3D12 plugin load against it) and the
# Vulkan symbols (so the source Vulkan plugin loads against it). The prebuilt
# 0.10.0 runtime is D3D12-only and is missing the 16 Vulkan symbols the plugin
# needs, so the game must run against this source-built runtime instead.
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend" FORCE)
set(REXGLUE_USE_D3D12 ON CACHE BOOL "Enable D3D12 graphics backend" FORCE)

# SDK source: default to the thirdparty/rexglue-sdk submodule (your fork,
# branch vsync-present-gate); override with -DREXGLUE_SDK_SOURCE=...
if(NOT REXGLUE_SDK_SOURCE OR NOT EXISTS "${REXGLUE_SDK_SOURCE}")
    set(REXGLUE_SDK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/rexglue-sdk")
endif()
message(STATUS "REXGLUE_SDK_SOURCE: ${REXGLUE_SDK_SOURCE}")
if(NOT EXISTS "${REXGLUE_SDK_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "SDK source not found at ${REXGLUE_SDK_SOURCE} - run git submodule update --init --recursive first")
endif()

# The SDK's src/core/memory.cpp SIMD byte-swap paths use SSSE3+ intrinsics
# (_mm_shuffle_epi8) without per-function target attributes, and the SDK's
# CMake compiles with a baseline x86-64 ISA - so enable a modern x86-64
# baseline for the whole tree here (the game target already uses -msse4.1).
add_compile_options(-msse4.2)

# Add PDB debug info to the Release build so crash dumps can be symbolized with
# cdb. -g emits CodeView debug info into the objects (does not change the -O3
# machine code, so the PDBs match the DLLs); /DEBUG makes the linker emit the
# .pdb. (Directory-level, so it inherits into the SDK's targets.)
add_compile_options($<$<CONFIG:RELEASE>:-g>)
# /DEBUG must reach lld-link via -Xlinker (a bare /DEBUG is read by the clang
# driver as a file path). Only Release is built in this wrapper.
add_link_options(-Xlinker /DEBUG)

add_subdirectory(${REXGLUE_SDK_SOURCE} "${CMAKE_BINARY_DIR}/sdk")

# ============================================================================
# rexgpu-fable2vulkan: the Fable 2 custom Vulkan renderer (this project).
#
# Lives in the game repo (src/renderer) but is built here, because it must
# compile against the SDK SOURCE tree with REXGLUE_USE_VULKAN=ON: it reuses the
# SDK's backend-agnostic guest-side graphics pipeline (recompiled here) and
# links the SDK's glslang / VMA / Vulkan-header targets, which only exist in a
# source build (the prebuilt 0.10.0 package is D3D12-only and ships no Vulkan
# headers or glslang headers). See plans/custom-vulkan-renderer.md.
#
# It recompiles the SDK's graphics pipeline sources (guest + SPIR-V + Vulkan
# backend) WITHOUT the SDK's own plugin_main.cpp (this project supplies its own
# rex_gpu_create / rex_gpu_abi_version exports), so the plugin re-binds the
# graphics system + command processor to the custom fable2::renderer classes.
# ============================================================================
if(REXGLUE_USE_VULKAN)
    set(FABLE2_RENDERER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    set(GFX "${REXGLUE_SDK_SOURCE}/src/graphics")

    # --- W1: the custom Vulkan foundation (vk_context, memory, pipeline cache, SPIRV-Tools, loader) ---
    set(FABLE2_VKFOUNDATION_SOURCES
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_function_table.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/memory_tracker.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/pipeline_cache.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/spirv_tools.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_context.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/render_pass_manager.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/descriptor_manager.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/pipeline_manager.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/texture_cache.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/render_target_cache.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/shared_memory.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/draw_dispatch.cpp)

    set(FABLE2VULKAN_SOURCES
        ${FABLE2_RENDERER_ROOT}/src/renderer/plugin_main.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/fable2_vulkan_graphics_system.cpp
        ${FABLE2_VKFOUNDATION_SOURCES}

        # --- SDK guest-side pipeline (reused) ---
        ${GFX}/flags.cpp
        ${GFX}/register_file.cpp
        ${GFX}/registers.cpp
        ${GFX}/pipeline/shader/shader.cpp
        ${GFX}/format/ucode.cpp
        ${GFX}/sampler_info.cpp
        ${GFX}/graphics_system.cpp
        ${GFX}/command_processor.cpp
        ${GFX}/frame_limiter.cpp
        ${GFX}/util/draw_extent_estimator.cpp
        ${GFX}/util/draw.cpp
        ${GFX}/packet_disassembler.cpp
        ${GFX}/primitive_processor.cpp
        ${GFX}/pipeline/render_target/cache.cpp
        ${GFX}/shared_memory.cpp
        ${GFX}/pipeline/texture/cache.cpp
        ${GFX}/pipeline/shader/interpreter.cpp
        ${GFX}/pipeline/shader/translator.cpp
        ${GFX}/pipeline/shader/translator_disasm.cpp

        # --- Xenos register-bank -> SPIR-V translation (reused) ---
        ${GFX}/pipeline/shader/spirv_builder.cpp
        ${GFX}/pipeline/shader/spirv.cpp
        ${GFX}/pipeline/shader/spirv_translator.cpp
        ${GFX}/pipeline/shader/spirv_translator_alu.cpp
        ${GFX}/pipeline/shader/spirv_translator_fetch.cpp
        ${GFX}/pipeline/shader/spirv_translator_memexport.cpp
        ${GFX}/pipeline/shader/spirv_translator_rb.cpp

        # --- SDK Vulkan host backend (reused until custom classes replace it) ---
        ${GFX}/vulkan/graphics_system.cpp
        ${GFX}/vulkan/deferred_command_buffer.cpp
        ${GFX}/vulkan/command_processor.cpp
        ${GFX}/vulkan/pipeline_cache.cpp
        ${GFX}/vulkan/primitive_processor.cpp
        ${GFX}/vulkan/render_target_cache.cpp
        ${GFX}/vulkan/shader.cpp
        ${GFX}/vulkan/shared_memory.cpp
        ${GFX}/vulkan/texture_cache.cpp
    )

    add_library(rexgpu-fable2vulkan SHARED ${FABLE2VULKAN_SOURCES})
    set_target_properties(rexgpu-fable2vulkan PROPERTIES
        EXPORT_NAME gpu-fable2vulkan
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(rexgpu-fable2vulkan PRIVATE
        ${FABLE2_RENDERER_ROOT}/src/renderer
        ${REXGLUE_SDK_SOURCE}
        ${REXGLUE_SDK_SOURCE}/thirdparty/renderdoc)
    target_compile_options(rexgpu-fable2vulkan PRIVATE -Wno-unused-variable)

    # Everything PRIVATE: the plugin has no consumers. Never relink the
    # OBJECT libraries; rexruntime already contains those objects.
    target_link_libraries(rexgpu-fable2vulkan PRIVATE
        rexruntime
        xxHash::xxhash
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler)

    # ------------------------------------------------------------------
    # fable2-vk-smoke: standalone console self-test for the VkContext
    # foundation (W1). Exercises instance + device + VMA + a real GPU buffer
    # round-trip + image alloc + on-disk pipeline-cache round-trip, and prints
    # a PASS/FAIL report + memory/cache stats. Built so it can run headless
    # (no window, no presentation) on any machine with a Vulkan loader.
    # ------------------------------------------------------------------
    add_executable(fable2-vk-smoke
        ${FABLE2_VKFOUNDATION_SOURCES}
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/frame_presenter.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_present_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_renderpass_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_pipeline_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_texture_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_rt_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_shared_blit_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_draw_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_shader_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_perf_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_robustness_smoke.cpp
        ${FABLE2_RENDERER_ROOT}/src/renderer/vulkan/vk_smoke_main.cpp)
    target_include_directories(fable2-vk-smoke PRIVATE
        ${FABLE2_RENDERER_ROOT}/src/renderer
        ${REXGLUE_SDK_SOURCE}
        ${REXGLUE_SDK_SOURCE}/thirdparty/renderdoc
        ${REXGLUE_SDK_SOURCE}/thirdparty/glslang)
    target_compile_options(fable2-vk-smoke PRIVATE -Wno-unused-variable)
    target_link_libraries(fable2-vk-smoke PRIVATE
        rexruntime
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler)
endif()
