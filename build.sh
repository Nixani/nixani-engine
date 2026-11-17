#!/bin/sh
# Configures and builds nixani-engine on Linux and macOS: build/release/nixani-engine
# Usage: ./build.sh [release|debug]
set -eu
cd "$(dirname "$0")"
config="${1:-release}"

# vcpkg: VCPKG_ROOT if set, else a checkout in ./vcpkg (setup.sh makes one).
: "${VCPKG_ROOT:=$PWD/vcpkg}"
export VCPKG_ROOT
[ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ] || { echo "vcpkg not found at $VCPKG_ROOT. Run ./setup.sh or set VCPKG_ROOT." >&2; exit 1; }

cmake --preset "posix-$config"
cmake --build --preset "posix-$config"
