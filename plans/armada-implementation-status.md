# Armada implementation checkpoint

Target: native Linux ARM64 + Vulkan on the user's AYN Odin 2 (16 GB).
Worktree: `armada/odin2-vulkan`; fork created at https://github.com/Rhonstin/Fable-2-Recomp.

## Verified

- SSH works without a password. Local SSH config selects `id_ed25519_proxmox` for
  `armada` and `192.168.2.210`; `ssh -o BatchMode=yes armada 'whoami; uname -m'`
  returns `armada` / `aarch64`.
- Odin runs Armada `20261012.fd0ed0f`, Fedora 44, 4 KB pages, Mesa Turnip 26.2.4, Adreno 740 Vulkan 1.4.354.
- SDK gitlink `3c02ca0bf994bfff771307e04209d8753342c97d` cloned recursively locally.
  Its existing runtime fixes are already baked in. Vendored libmspack needs the `.git` marker created by `prepare_runtime_sdk.py`.
- SDK native builds in rootless Fedora 44 container `fable2-build` on Odin;
  bind mount `/var/home/armada/Projects/Fable-2-Recomp` at `/work`.
- SDK Linux compilation fixes: guest frame counters outside Windows-only block in presenter.h;
  Windows /DEF linker argument conditional on WIN32; wrapper SSE and /DEBUG flags platform-dependent.
- Native SDK runtime and GPU plugin built on Odin. Copies in local `out/sdk-arm64` include generated version.h.
- Physical GPU test succeeded on Odin, both native-built and cross-built probes:
  `REXGLUE_VULKAN_DEVICE_OK`, device Adreno 740 (not llvmpipe).
  Probe also loads the actual `xenos` Vulkan plugin.
- Four ARM64 synthetic native tests ran successfully on Odin: audio sink, frame limiter,
  guest frame meter, XEX verification utility. Seven existing Python source-only tests pass.
- SDK PPC instruction suite generated from all 166 assembly fixtures with the host
  code generator, cross-compiled for ARM64, and executed on Odin against the same runtime:
  **1458 test cases / 5725 assertions passed**. Exercises scalar/vector instructions
  and the real guest memory allocator. Log: `out/armada-audit/ppc-tests.log`.
- Supplied ISO extracted locally with repository's .NET 8 x360extract into ignored root `default.xex` and `data/`.
  This is NOT GOTY: Russian retail version 0.0.0.8, media ID 6BC95506, build date 2008-09-13.
  Exact XEX SHA256: `f774bb9d4fb6196eb6867c9d38bd00569d5fa5a0d57cad74108225a73fff0c4a`.
- Created separate `config/armada/retail.toml`; do not reuse GOTY function addresses/hooks.
  Native codegen succeeds with explicit genuine retail entry hints and no unresolved calls.
  Generated outputs in `generated/retail` (~300 MB; 583 output files plus metadata).
  Configuration-local generated glue in `config/armada/generated` is ignored; original GOTY glue restored.

## Native launch evidence

- Full ARM64 ELF linked successfully with Clang 22 / lld, Release `-O2 -DNDEBUG`.
  Second build includes verified retail CRT setjmp/longjmp addresses;
  build ID `bc7cc874a2f236715873899b46f8094e2fe9385e`.
- Complete extracted game content transferred to Odin; remote XEX SHA-256 matches.
- First native launch on host Armada: SDL Wayland, Turnip Adreno 740,
  Vulkan swapchain created, guest memory at 4/8 GB host bases,
  function table registered, SDL input and six-channel 48 kHz speaker endpoint initialized.
- Startup then aborted on an unregistered indirect guest function at `0x82C982D8`.
  No guest image or playable scene observed yet. Log: local ignored
  `out/armada-audit/launch-001.log`.
- The missing function has a concrete vtable reference at `0x82013C18`.
  Inspection found further omitted methods/getters in `.rdata` pointer runs;
  explicit hints now added with reference addresses. Exception landing pads
  inside larger routines are excluded. Regeneration/build/testing in progress.
- Temporary uinput controller creates successfully on Odin and accepts/release
  test button events. In-game input is still unverified.

## In progress / next actions

- Cross-building native game with Clang 22 on PC against exact Fedora ARM64 rootfs exported from build container.
  Sysroot: `out/sysroot-arm64`; toolchain: `tools/armada/aarch64-toolchain.cmake`.
  Configure `tools/armada` to `out/build/game-arm64` with FABLE2_PREBUILT_SDK pointing at `out/sdk-arm64`.
  Current config Release `-O2 -DNDEBUG`, game build `cmake --build out/build/game-arm64 --target fable_2 -j 4`.
  Compilation log: `out/armada-audit/build-game-arm64.log`.
- Corrected imported SDK include closure (imgui/renderdoc), protected base-constructor factory,
  and toolchain try_compile propagation. Inspect live build before restarting it.
- Native app lives in `src/armada/main.cpp`. Forces Vulkan, exact-XEX validation,
  separate XDG saves/config/cache under `fable2-native/retail-ru`.
  Optional `FABLE2_CAPTURE_DIR` periodically captures actual guest output to PNG every 5 seconds.
  Window's added inline `graphics_presenter()` accessor has no ABI layout change;
  included in `thirdparty/rexglue-sdk-armada.patch`.
- Source/contents copied to remote project; executable hash verified remotely.
  Launch game on host OS, not inside build container, to use host Turnip/desktop/input/audio.
- Remote GUI session environment from systemd user manager:
  DISPLAY=:0, WAYLAND_DISPLAY=wayland-0, XDG_RUNTIME_DIR=/run/user/1000,
  XAUTHORITY=/run/user/1000/xauth_hdEUjZ, DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus.
  Read current environment again before relying on XAUTHORITY (may change).
- `/dev/uinput` is writable to armada; `tools/armada/gamepad.py` supplies temporary test controller
  with JSON stdin commands, e.g. {"button":"a","hold":0.2}; device and commands work,
  game integration unverified.
- Matching binary + librexruntime.so + librexgpu-xenos.so staged to remote build bin.
  Launch via `systemd-run --user` using `tools/armada/run-native.sh` in the desktop environment.
  Inspect logs/screenshots and debug actual faults; passing compile/probe is not game readiness.

## Remaining

Native gameplay startup, missing guest call/switch fixes if any, rendering correctness,
input/audio, save/load, performance, clean package/Steam shortcut, reproducible build validation.
No claim of playable native game yet. The native executable launches but guest startup currently fails.
No game files are committed or to be published. No WIP source commits pushed yet.
