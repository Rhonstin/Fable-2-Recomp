#!/usr/bin/env bash
set -euo pipefail
fable_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$fable_root"
if [[ $(uname -m) != aarch64 ]]; then
    printf 'This script builds on ARM64. For cross builds use aarch64-toolchain.cmake and FABLE2_PREBUILT_SDK.\n' >&2
    exit 1
fi
fable_expected=f774bb9d4fb6196eb6867c9d38bd00569d5fa5a0d57cad74108225a73fff0c4a
fable_actual=$(sha256sum default.xex | cut -d ' ' -f 1)
if [[ "$fable_actual" != "$fable_expected" ]]; then
    printf 'This profile requires Russian retail 0.0.0.8 XEX SHA-256 %s; found %s\n' "$fable_expected" "$fable_actual" >&2
    exit 1
fi
"$fable_root/tools/armada/prepare-sdk.sh"
fable_build_dir=${FABLE2_BUILD_DIR:-"$fable_root/out/build/native-arm64"}
cmake -S tools/armada -B "$fable_build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DFABLE2_BUILD_GAME=OFF -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG'
cmake --build "$fable_build_dir" --target rexglue fable2_vulkan_probe \
    --parallel "${FABLE2_BUILD_JOBS:-3}"
"$fable_root/thirdparty/rexglue-sdk/out/linux-arm64/rexglue" codegen config/armada/retail.toml
cmake -S tools/armada -B "$fable_build_dir" -DFABLE2_BUILD_GAME=ON
cmake --build "$fable_build_dir" --target fable_2 --parallel "${FABLE2_BUILD_JOBS:-3}"
printf 'Native game: %s/bin/fable_2\n' "$fable_build_dir"
