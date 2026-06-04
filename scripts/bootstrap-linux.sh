#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

missing=()

require_cmd() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        missing+=("$name")
    fi
}

echo "Syncing submodule URLs..."
git submodule sync --recursive

echo "Initializing submodules..."
jobs="$(nproc 2>/dev/null || echo 4)"
git submodule update --init --recursive --jobs "$jobs"

require_cmd git
require_cmd cmake
require_cmd ninja
require_cmd pkg-config
require_cmd clang-20
require_cmd clang++-20
require_cmd ld.lld-20

if [[ ! -f third_party/rexglue-sdk/CMakeLists.txt ]]; then
    echo "Missing third_party/rexglue-sdk/CMakeLists.txt after submodule init." >&2
    exit 1
fi

if ((${#missing[@]})); then
    echo
    echo "Missing required build tools: ${missing[*]}" >&2
    echo "On Ubuntu 24.04, install the core toolchain and Linux headers with:" >&2
    echo "  sudo apt install git cmake ninja-build clang-20 lld-20 pkg-config build-essential libgtk-3-dev libx11-xcb-dev libvulkan-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev" >&2
    echo "If clang-20 is unavailable from Ubuntu apt, install it from https://apt.llvm.org/." >&2
    exit 1
fi

echo
echo "Bootstrap checks passed."
echo "Configure with:"
echo "  cmake --preset linux-relwithdebinfo -DSKATE3_GAME_DATA_ROOT=\"\$PWD/game\""
