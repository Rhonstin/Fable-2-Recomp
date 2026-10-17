cmake_minimum_required(VERSION 3.25)
project(fable2_armada LANGUAGES C CXX)

get_filename_component(FABLE2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Vulkan renderer" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "D3D12 renderer" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Tracy profiler" FORCE)
option(FABLE2_BUILD_GAME "Build the game after running codegen" ON)
set(FABLE2_GENERATED_DIR "${FABLE2_ROOT}/generated/retail" CACHE PATH "Recompiled guest sources")
set(FABLE2_XEX_SHA256 "f774bb9d4fb6196eb6867c9d38bd00569d5fa5a0d57cad74108225a73fff0c4a" CACHE STRING "Exact XEX used by codegen")
set(FABLE2_PREBUILT_SDK "" CACHE PATH "Matching native SDK artifacts for cross-compilation")
set(FABLE2_PPC_TEST_DIR "" CACHE PATH "Instruction test sources from generate-ppc-tests.py")

set(FABLE2_SDK "${FABLE2_ROOT}/thirdparty/rexglue-sdk")
if(FABLE2_PREBUILT_SDK)
    add_library(rexruntime SHARED IMPORTED GLOBAL)
    add_library(rex::runtime ALIAS rexruntime)
    set_target_properties(rexruntime PROPERTIES
        IMPORTED_LOCATION "${FABLE2_PREBUILT_SDK}/librexruntime.so"
        INTERFACE_INCLUDE_DIRECTORIES "${FABLE2_SDK}/include;${FABLE2_PREBUILT_SDK}/include;${FABLE2_SDK}/thirdparty/fmt/include;${FABLE2_SDK}/thirdparty/spdlog/include;${FABLE2_SDK}/thirdparty/simde;${FABLE2_SDK}/thirdparty/sdl3/include;${FABLE2_SDK}/thirdparty/vulkan-headers/include;${FABLE2_SDK}/thirdparty/vulkan-memory-allocator/include;${FABLE2_SDK}/thirdparty/spirv-tools/include;${FABLE2_SDK}/thirdparty/tomlplusplus/include"
        INTERFACE_COMPILE_DEFINITIONS "REX_PLATFORM_LINUX=1;REX_HAS_VULKAN=1;SPDLOG_COMPILED_LIB;SPDLOG_FMT_EXTERNAL")
    target_include_directories(rexruntime INTERFACE
        "${FABLE2_SDK}/thirdparty/imgui" "${FABLE2_SDK}/thirdparty/renderdoc")
    add_library(rex::gpu-xenos SHARED IMPORTED GLOBAL)
    set_target_properties(rex::gpu-xenos PROPERTIES IMPORTED_LOCATION "${FABLE2_PREBUILT_SDK}/librexgpu-xenos.so")
    set(REXGLUE_SHARE_DIR "${FABLE2_SDK}/src/ui")
    include("${FABLE2_SDK}/cmake/rexglue_helpers.cmake")
else()
    add_subdirectory("${FABLE2_SDK}" sdk)
endif()

add_executable(fable2_vulkan_probe "${FABLE2_ROOT}/src/armada/vulkan_probe.cpp")
target_link_libraries(fable2_vulkan_probe PRIVATE rex::runtime)
target_include_directories(fable2_vulkan_probe PRIVATE "${FABLE2_SDK}/thirdparty/renderdoc")
rexglue_apply_target_settings(fable2_vulkan_probe)
if(TARGET rexgpu-xenos)
    add_dependencies(fable2_vulkan_probe rexgpu-xenos)
    set(FABLE2_PROBE_PLUGIN rexgpu-xenos)
else()
    set(FABLE2_PROBE_PLUGIN rex::gpu-xenos)
endif()
add_custom_command(TARGET fable2_vulkan_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${FABLE2_PROBE_PLUGIN}> $<TARGET_FILE_DIR:fable2_vulkan_probe>
    VERBATIM)

if(FABLE2_BUILD_GAME)
    if(NOT EXISTS "${FABLE2_GENERATED_DIR}/sources.cmake")
        message(FATAL_ERROR "Run rexglue codegen config/armada/retail.toml before configuring the game")
    endif()
    include("${FABLE2_GENERATED_DIR}/sources.cmake")
    # A standalone title registers through PPCImageConfig.func_mappings in
    # Runtime::Setup. The separate 60k-call registrar is for module loading.
    if(NOT EXISTS "${FABLE2_GENERATED_DIR}/module_registry.cpp")
        list(FILTER GENERATED_SOURCES EXCLUDE REGEX "/fable_2_register\\.cpp$")
    endif()
    add_library(fable2_guest OBJECT ${GENERATED_SOURCES})
    target_link_libraries(fable2_guest PRIVATE rex::runtime)
    target_include_directories(fable2_guest PRIVATE "${FABLE2_GENERATED_DIR}")
    target_precompile_headers(fable2_guest PRIVATE "${FABLE2_GENERATED_DIR}/fable_2_pch.h")
    target_compile_options(fable2_guest PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
    rexglue_apply_target_settings(fable2_guest)

    add_executable(fable_2 "${FABLE2_ROOT}/src/armada/main.cpp")
    target_include_directories(fable_2 PRIVATE "${FABLE2_ROOT}" "${FABLE2_GENERATED_DIR}")
    target_compile_definitions(fable_2 PRIVATE FABLE2_XEX_SHA256="${FABLE2_XEX_SHA256}")
    target_link_libraries(fable_2 PRIVATE fable2_guest rex::runtime)
    rexglue_configure_target(fable_2 GPU_PLUGINS xenos)
endif()

if(FABLE2_PPC_TEST_DIR)
    add_subdirectory("${FABLE2_SDK}/thirdparty/catch2" catch2 EXCLUDE_FROM_ALL)
    add_executable(fable2_ppc_tests
        "${FABLE2_PPC_TEST_DIR}/ppc_test_functions.cpp"
        "${FABLE2_PPC_TEST_DIR}/ppc_test_cases.cpp")
    target_include_directories(fable2_ppc_tests PRIVATE "${FABLE2_PPC_TEST_DIR}")
    target_link_libraries(fable2_ppc_tests PRIVATE rex::runtime Catch2::Catch2WithMain)
    target_compile_options(fable2_ppc_tests PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
    rexglue_apply_target_settings(fable2_ppc_tests)
endif()

# Linux uses the matching runtime next to each executable, including the probe.
foreach(app fable2_vulkan_probe fable_2 fable2_ppc_tests)
    if(TARGET ${app})
        set_target_properties(${app} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
        add_custom_command(TARGET ${app} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:${app}>
            VERBATIM)
    endif()
endforeach()
