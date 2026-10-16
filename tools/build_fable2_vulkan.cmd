@echo off
rem ===========================================================================
rem Build the Fable 2 custom Vulkan renderer plugin (rexgpu-fable2vulkan.dll).
rem
rem This is the custom high-performance Vulkan backend described in
rem plans/custom-vulkan-renderer.md. It is built from the SDK SOURCE tree with
rem REXGLUE_USE_VULKAN=ON (via the tools/vulkan_sdk wrapper, which also builds
rem a source rexruntime that exports the Vulkan symbols the plugin needs), and
rem compiles the SDK's backend-agnostic guest-side graphics pipeline together
rem with this project's custom graphics-system / command-processor classes in
rem src/renderer.
rem
rem Result: rexgpu-fable2vulkan.dll (Release) + its deps (a source rexruntime,
rem glslang, SPIRV, VMA, the Vulkan loader). The plugin is then staged next to
rem the game exe so it can be selected with:
rem   --gpu_plugin=fable2-vulkan     or     [gpu] renderer = "fable2"
rem
rem The stock renderer is unaffected: rexgpu-xenos.dll (D3D12) stays the
rem default. Both plugins coexist and are A/B-able with no rebuild.
rem
rem Toolchain: clang + lld + Ninja (same as build_sdk_vulkan.cmd). This is a
rem LONG build on a clean tree (fetches + compiles the pinned Vulkan deps).
rem
rem Usage:
rem   tools\build_fable2_vulkan.cmd              build + stage to win-amd64-release
rem   tools\build_fable2_vulkan.cmd <builddir>   stage to out\build\<builddir>
rem ===========================================================================
setlocal
set "REPO=%~dp0.."
set "WRAPPER=%REPO%\tools\vulkan_sdk"
set "OUT=%REPO%\out\build\fable2-vulkan"
set "BUILDDIR=%~1"
if "%BUILDDIR%"=="" set "BUILDDIR=win-amd64-release"
set "STAGE=%REPO%\out\build\%BUILDDIR%"

rem SDK source: the thirdparty\rexglue-sdk submodule.
set "SDKSRC=%REPO%\thirdparty\rexglue-sdk"
if not exist "%SDKSRC%\CMakeLists.txt" (
    echo SDK source not found; run: git submodule update --init --recursive
    exit /b 1
)
set "REXGLUE_SDK_SOURCE=%SDKSRC%"

rem LLVM: prefer clang++ already on PATH, else the default install location.
where clang++ >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" set "LLVM=C:\Program Files\LLVM\bin"
    if not defined LLVM (
        echo Error: LLVM not found on PATH and no clang++.exe at C:\Program Files\LLVM\bin 1>&2
        echo        Install LLVM - https://llvm.org - or add its bin\ to PATH. 1>&2
        exit /b 1
    )
)
if defined LLVM set "PATH=%LLVM%;%PATH%"
set "PATH=%USERPROFILE%\bin;%PATH%"
where ninja >nul 2>nul
if errorlevel 1 (
    for %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe") do set "PATH=%%~dpd;%PATH%"
)

echo === Wrapper: %WRAPPER%
echo === Build dir: %OUT%

echo === Configuring (reuses already-fetched deps; fast unless deps changed) ===
cmake -S "%WRAPPER%" -B "%OUT%" -G "Ninja Multi-Config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_STANDARD=23 -DCMAKE_CONFIGURATION_TYPES=Release -DREXGLUE_SDK_SOURCE="%SDKSRC%"
if errorlevel 1 exit /b 1

echo === Building rexgpu-fable2vulkan + rexruntime Release ===
cmake --build "%OUT%" --config Release --target rexgpu-fable2vulkan --target rexruntime
if errorlevel 1 (
    echo BUILD FAILED 1>&2
    exit /b 1
)

rem Locate the built plugin DLL.
set "PLUGIN="
for /f "delims=" %%f in ('dir /s /b "%OUT%\*rexgpu-fable2vulkan.dll" 2^>nul') do set "PLUGIN=%%f"
if "%PLUGIN%"=="" (
    echo Error: rexgpu-fable2vulkan.dll not produced under %OUT% 1>&2
    exit /b 1
)
echo === Built: %PLUGIN%

rem Stage the plugin (and the source-built runtime deps it needs) next to the
rem game exe so --gpu_plugin=fable2-vulkan can load it.
if not exist "%STAGE%" (
    echo Warning: build dir %STAGE% does not exist; not staging. Build the game first (build.cmd -release).
    exit /b 0
)
copy /y "%PLUGIN%" "%STAGE%\rexgpu-fable2vulkan.dll"
rem The plugin loads the source-built rexruntime (Vulkan-capable). Stage its
rem Release runtime DLLs so the source runtime is present next to the exe.
for %%d in ("%OUT%\sdk\thirdparty\rexglue-sdk\out\*rexruntime.dll") do copy /y "%%d" "%STAGE%\" >nul
for %%d in ("%OUT%\sdk\win-amd64\*rexruntime.dll") do copy /y "%%d" "%STAGE%\" >nul
echo === Staged rexgpu-fable2vulkan.dll into %STAGE%
echo === Select it with:  fable_2.exe --gpu_plugin=fable2-vulkan
exit /b 0
