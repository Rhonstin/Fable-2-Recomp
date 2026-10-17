#!/usr/bin/env bash
set -euo pipefail
fable_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fable_sdk="$fable_root/thirdparty/rexglue-sdk"
git -C "$fable_root" submodule update --init --recursive --jobs 4
python3 "$fable_root/tools/prepare_runtime_sdk.py" "$fable_sdk"
fable_patch="$fable_root/thirdparty/rexglue-sdk-armada.patch"
if git -C "$fable_sdk" apply --reverse --check "$fable_patch" 2>/dev/null; then
    printf 'Armada SDK patch is already applied.\n'
else
    git -C "$fable_sdk" apply --check "$fable_patch"
    git -C "$fable_sdk" apply "$fable_patch"
fi
