#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Building Linux Debug..."
"$repo_root/scripts/build-linux-debug.sh"

echo "Building Linux Release..."
"$repo_root/scripts/build-linux-release.sh"

echo "Building Windows Debug..."
"$repo_root/scripts/build-windows-debug.sh"

echo "Building Windows Release..."
"$repo_root/scripts/build-windows-release.sh"

echo "All build configurations completed."
