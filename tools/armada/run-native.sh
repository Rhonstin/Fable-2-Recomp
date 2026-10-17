#!/usr/bin/env bash
set -euo pipefail
fable_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fable_binary_dir=${FABLE2_BINARY_DIR:-"$fable_root/out/build/game-arm64/bin"}
fable_game_root=${FABLE2_GAME_ROOT:-"$fable_root"}
if [[ ! -x "$fable_binary_dir/fable_2" ]]; then
    printf 'Native executable is missing: %s/fable_2\n' "$fable_binary_dir" >&2
    exit 1
fi
if [[ ! -f "$fable_game_root/default.xex" || ! -f "$fable_game_root/data/dir.manifest" ]]; then
    printf 'Set FABLE2_GAME_ROOT to the folder containing default.xex and data/.\n' >&2
    exit 1
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-"/run/user/$(id -u)"}
export REX_VULKAN_DEVICE=${REX_VULKAN_DEVICE:-0}
exec "$fable_binary_dir/fable_2" --game_data_root "$fable_game_root" \
    --window_width 1280 --window_height 720 "$@"
