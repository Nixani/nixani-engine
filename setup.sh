#!/bin/sh
# One-time setup on Linux and macOS: submodules, the Python venv used for code generation, and vcpkg.
# Needs: git, Python 3 (with venv), CMake 3.25+, Ninja, pkg-config, a C/C++ compiler, perl and make.
#   Debian / Ubuntu:  sudo apt install build-essential cmake ninja-build pkg-config python3-venv git curl zip unzip tar
#   macOS:            xcode-select --install && brew install cmake ninja pkg-config
set -eu
cd "$(dirname "$0")"

git submodule update --init --depth 1
git -C third_party/chiaki-ng submodule update --init --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl

[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install --quiet --disable-pip-version-check protobuf grpcio-tools

if [ -z "${VCPKG_ROOT:-}" ] && [ ! -x vcpkg/vcpkg ]; then
	[ -d vcpkg ] || git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
	./vcpkg/bootstrap-vcpkg.sh -disableMetrics
fi
echo 'Ready. Build with ./build.sh'
