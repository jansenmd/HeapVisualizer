#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
configuration="${1:-debug}"

case "$configuration" in
    debug)
        build_dir="$repo_root/build/linux-debug"
        "$repo_root/scripts/build-linux-debug.sh"
        ;;
    release)
        build_dir="$repo_root/build/linux-release"
        "$repo_root/scripts/build-linux-release.sh"
        ;;
    *)
        echo "Usage: $0 [debug|release]" >&2
        exit 1
        ;;
esac

hook_library="$build_dir/libheap_allocation_hooks.so"
test_application="$build_dir/HeapAllocationTest"

if [[ ! -f "$hook_library" ]]; then
    echo "Missing preload library: $hook_library" >&2
    exit 1
fi

if [[ ! -x "$test_application" ]]; then
    echo "Missing allocation test application: $test_application" >&2
    exit 1
fi

echo "Launching HeapAllocationTest with LD_PRELOAD=$hook_library"
LD_PRELOAD="$hook_library" "$test_application"
