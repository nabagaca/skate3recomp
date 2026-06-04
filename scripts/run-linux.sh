#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/out/build/linux-relwithdebinfo"
rexglue_lib_dir="$repo_root/third_party/rexglue-sdk/out/linux-amd64"
game_dir="${SKATE3_GAME_DATA_ROOT:-$repo_root/game}"
exe="$build_dir/skate3"

if [[ ! -x "$exe" ]]; then
    echo "Missing executable: $exe" >&2
    echo "Build it with:" >&2
    echo "  cmake --build --preset linux-relwithdebinfo --parallel" >&2
    exit 1
fi

if [[ ! -d "$rexglue_lib_dir" ]]; then
    echo "Missing rexglue runtime library directory: $rexglue_lib_dir" >&2
    echo "Build rexglue with:" >&2
    echo "  cmake --build --preset linux-relwithdebinfo --target generate-all --parallel" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$rexglue_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

has_game_root_arg=0
has_input_backend_arg=0
has_mnk_mode_arg=0
for arg in "$@"; do
    case "$arg" in
        --game_data_root|--game_data_root=*)
            has_game_root_arg=1
            ;;
        --input_backend|--input_backend=*)
            has_input_backend_arg=1
            ;;
        --mnk_mode|--no-mnk_mode)
            has_mnk_mode_arg=1
            ;;
    esac
done

default_args=()
if [[ "$has_game_root_arg" -eq 0 ]]; then
    default_args+=(--game_data_root="$game_dir")
fi
if [[ "$has_input_backend_arg" -eq 0 ]]; then
    default_args+=(--input_backend=sdl)
fi
if [[ "$has_mnk_mode_arg" -eq 0 && "${SKATE3_MNK:-0}" == "1" ]]; then
    default_args+=(--mnk_mode)
fi

exec "$exe" "${default_args[@]}" "$@"
