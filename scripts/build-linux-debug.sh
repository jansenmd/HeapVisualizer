#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_args=()

if [[ -n "${QT_ROOT:-}" ]]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=${QT_ROOT}")
fi

cd "$repo_root"

echo "Configuring Linux Debug..."
cmake --preset linux-debug "${cmake_args[@]}"

echo "Building Linux Debug..."
cmake --build --preset build-linux-debug
