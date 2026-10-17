# Native Fable II on Armada

This work targets Linux ARM64 with the ReXGlue Vulkan renderer. It is a native
ELF executable, with no FEX or Proton in the game execution path.

## Executable revision

The supplied disc is Russian retail **0.0.0.8**, media ID `6BC95506`, not GOTY.
Its 20,758,528-byte `default.xex` has SHA-256:

```text
f774bb9d4fb6196eb6867c9d38bd00569d5fa5a0d57cad74108225a73fff0c4a
```

`config/armada/retail.toml` discovers this revision's actual functions. Its
retail tail-call entry hints and CRT setjmp/longjmp addresses were inspected
in the decoded image. GOTY hooks, switch tables and hot-function replacements
must not be transplanted by address. This separate profile does not change the
existing Windows GOTY build or mark retail as compatible with its code group.

Keep the extracted `default.xex` and full `data/` beside the source checkout
for building. None of these files belongs in Git or a published binary archive.
The existing `tools/x360extract` .NET 8 CLI can extract the supplied image.

## Build on ARM64

Build in a Fedora 44 development environment; the `Containerfile` supplies
its required compiler and development packages. Build the image with Podman,
mount the full checkout at `/work`, then execute:

```bash
tools/armada/build-native.sh
```

The script checks the exact executable hash, initializes the pinned SDK,
applies the tracked Armada patch idempotently, builds the code generator,
generates retail code and builds the native game. A default of three compile
jobs limits peak memory; `FABLE2_BUILD_JOBS` can override it.

For cross builds, first build the matching SDK natively and supply its
`librexruntime.so`, `librexgpu-xenos.so` and generated `include/rex/version.h`
in a directory passed as `FABLE2_PREBUILT_SDK`. Use
`aarch64-toolchain.cmake` with `FABLE2_ARM64_SYSROOT` pointing at the same
ARM64 Fedora development environment's exported root filesystem. Codegen
runs on the build host; the generated guest code compiles for ARM64.

Standalone entrypoint registration uses the existing
`PPCImageConfig.func_mappings` table through `Runtime::Setup`; an unused giant
module-registration translation unit is omitted only for a single-module
title. No guest functions are removed from that table.

## Run on the host OS

Use the host Armada Vulkan loader/Turnip driver, desktop, audio server and
controller. The development container is for compilation. Put the executable
and both matching SDK shared libraries in the same directory, then run:

```bash
FABLE2_BINARY_DIR=/path/to/native/bin tools/armada/run-native.sh
```

`FABLE2_GAME_ROOT` overrides the folder containing `default.xex` and `data/`.
The launcher defaults to `out/build/game-arm64/bin` for cross-built artifacts;
the native build script reports its own output directory.
Saves/config/cache are stored under the corresponding XDG directories in
`fable2-native/retail-ru`, so an update of binaries does not replace saves.

Native Steam shortcuts must launch this script/executable without forcing a
Proton compatibility tool. Confirm the selected GPU is Adreno rather than the
software llvmpipe device. The initial settings use a 1280×720 presentation
window; actual guest render resolution is verified from frame captures.

## Hardware diagnostics

`fable2_vulkan_probe` loads the actual GPU plugin and creates a ReXGlue Vulkan
device with GPU-emulation feature requirements. It rejects CPU rendering.
Success is `REXGLUE_VULKAN_DEVICE_OK`. This alone does not prove swapchain or
gameplay correctness.

Set `FABLE2_CAPTURE_DIR` when starting the game to capture actual guest frames
as PNG every five seconds for remote diagnosis. Capture is disabled when the
variable is absent. `gamepad.py` creates a temporary uinput controller and
accepts JSON lines on stdin; this requires permission to open `/dev/uinput`.
It releases buttons/axes and destroys the device when stopped normally.

The SDK's existing PowerPC instruction fixtures can also run on the real ARM64
runtime. Assemble/generate on the x86 build host (the SDK's bundled binutils
are host tools):

```bash
python3 tools/armada/generate-ppc-tests.py \
  --rexglue thirdparty/rexglue-sdk/out/linux-amd64/rexglue
```

Configure a separate `tools/armada` build with the ARM64 toolchain, matching
prebuilt SDK and sysroot, `FABLE2_BUILD_GAME=OFF`, and
`FABLE2_PPC_TEST_DIR=$PWD/out/ppc-tests/generated`. Build `fable2_ppc_tests`,
copy it with the matching runtime to Odin, and execute it there. This exercises
actual scalar/vector instruction results and guest memory; it requires no ROM.
It does not execute the ARM64 runner on the x86 build host during configuration.

Read `plans/armada-implementation-status.md` for the verified state and
remaining work. Compilation, GPU-device initialization and unit tests do not
by themselves establish a playable game.
