#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_args=()

normalize_qt_root() {
    local qt_root="$1"

    if [[ "$qt_root" =~ ^[A-Za-z]:[\\/].* ]] && command -v wslpath >/dev/null 2>&1; then
        wslpath -u "$qt_root"
    else
        printf '%s\n' "$qt_root"
    fi
}

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "Missing MinGW-w64 cross-compiler: x86_64-w64-mingw32-g++" >&2
    echo "Install it on Ubuntu/Debian/WSL with:" >&2
    echo "  sudo apt update && sudo apt install mingw-w64" >&2
    exit 1
fi

if [[ -n "${QT_ROOT:-}" ]]; then
    qt_root="$(normalize_qt_root "$QT_ROOT")"
    cmake_args+=("-DCMAKE_PREFIX_PATH=${qt_root}")
else
    echo "QT_ROOT is not set. CMake will search standard paths for a Windows MinGW Qt kit."
    echo "If configure fails, install Qt for Windows with the MinGW 64-bit kit and set QT_ROOT."
fi

cd "$repo_root"

echo "Configuring Windows Release..."
cmake --preset windows-mingw-release "${cmake_args[@]}"

echo "Building Windows Release..."
cmake --build --preset build-windows-release
