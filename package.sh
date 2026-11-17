#!/bin/sh
# Packs the engine into dist/nixani-engine on Linux and macOS: the program, the licence texts and
# where the source is. A client program ships that folder as it is.
# Usage: ./package.sh [--skip-build]
set -eu
cd "$(dirname "$0")"

[ "${1:-}" = "--skip-build" ] || ./build.sh release
version=$(sed -n 's/^project(nixani-engine VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo 'Could not read the version from CMakeLists.txt.' >&2; exit 1; }

bin=build/release/nixani-engine
out=dist/nixani-engine
rm -rf "$out"
mkdir -p "$out"
cp "$bin" "$out/"

# The program must run on a machine that has only the system itself: nothing may be loaded from
# a library that is not part of it.
if [ "$(uname)" = "Darwin" ]; then
	strip -x "$out/nixani-engine"
	foreign=$(otool -L "$out/nixani-engine" | tail -n +2 | awk '{print $1}' | grep -v -E '^(/usr/lib/|/System/Library/)' || true)
else
	strip --strip-unneeded "$out/nixani-engine"
	foreign=$(ldd "$out/nixani-engine" | awk '{print $1}' | grep -v -E '^(linux-vdso\.so|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libresolv\.so|/lib.*/ld-linux|ld-linux)' || true)
fi
[ -z "$foreign" ] || { echo "The package would be incomplete, the program needs:" >&2; echo "$foreign" >&2; exit 1; }

cat > "$out/LICENSE.txt" <<EOF
Nixani Engine $version

Nixani Engine is free software: you can redistribute it and/or modify it under the terms of the
GNU Affero General Public License, version 3. The full text is in LICENSE-AGPL-3.0.txt next to
this file. It comes with no warranty.

Source code of this version: https://github.com/Nixani/nixani-engine (tag v$version)

It is built on chiaki-ng (https://github.com/streetpea/chiaki-ng, AGPL-3.0 with an OpenSSL
exception). The other bundled components and their licence texts are in THIRD-PARTY-NOTICES.txt.

The engine is a separate program. The program that starts it only exchanges messages with it
(PROTOCOL.md in the source) and is not part of it.

An unofficial project. PlayStation, PS4, PS5 and DualSense are trademarks of
Sony Interactive Entertainment, which is not affiliated with it.
EOF
cp LICENSE "$out/LICENSE-AGPL-3.0.txt"

share=$(ls -d vcpkg_installed/*/share | grep -v -E '/(vcpkg)/' | head -n 1)
chiaki=third_party/chiaki-ng
notices="$out/THIRD-PARTY-NOTICES.txt"
echo "Nixani Engine $version includes the components below, each under its own licence." > "$notices"
add() { # name, licence file
	[ -f "$2" ] || { echo "Licence text missing for $1: $2" >&2; exit 1; }
	{
		printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1"
		cat "$2"
	} >> "$notices"
}
add 'chiaki-ng (libchiaki)' "$chiaki/LICENSES/AGPL-3.0-only-OpenSSL.txt"
add 'curl (in libchiaki)' "$chiaki/third-party/curl/COPYING"
add 'nanopb (in libchiaki)' "$chiaki/third-party/nanopb/LICENSE.txt"
add 'Jerasure (in libchiaki)' "$chiaki/third-party/jerasure/License.txt"
add 'GF-Complete (in libchiaki)' "$chiaki/third-party/gf-complete/License.txt"
add 'OpenSSL' "$share/openssl/copyright"
add 'Opus' "$share/opus/copyright"
add 'json-c' "$share/json-c/copyright"
add 'libevent' "$share/libevent/copyright"
add 'MiniUPnPc' "$share/miniupnpc/copyright"

echo "Packed $(ls "$out" | wc -l | tr -d ' ') files into $out"
