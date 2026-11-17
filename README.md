# Nixani Engine

A Remote Play engine for PS4 and PS5 consoles on Windows, Linux and macOS: one small program without a window
that finds consoles, links with them, wakes them and runs the Remote Play connection, on the
local network or over the internet through PSN.

It is built on [chiaki-ng](https://github.com/streetpea/chiaki-ng)'s libchiaki and puts it behind
a plain message protocol on standard input / output ([PROTOCOL.md](PROTOCOL.md)). A client
program starts the engine, sends commands and controller states, and receives the compressed
video frames, the decoded audio, haptics and the console's events. Decoding the video, drawing
it, playing the sound and reading the controller are up to the client, in whatever language and
under whatever licence it likes.

## Build (Windows)

```powershell
.\setup.ps1    # once: submodules, Python venv for code generation, vcpkg
.\build.ps1    # build\release\nixani-engine.exe and the DLLs it needs
.\package.ps1  # dist\nixani-engine: everything needed to ship it, with the licence texts
```

Needs git, Python 3 and Visual Studio 2022 Build Tools with the C++ and "C++ Clang tools"
components (libchiaki needs clang-cl). `VCPKG_ROOT` is used if set, otherwise `setup.ps1` clones
vcpkg into the folder.

`nixani-engine.exe --version` prints the version; started without a client on its pipes it has
nothing to do.

## Build (Linux, macOS)

```sh
./setup.sh     # once: submodules, Python venv for code generation, vcpkg
./build.sh     # build/release/nixani-engine
./package.sh   # dist/nixani-engine: the program with the licence texts
```

Needs git, Python 3 with venv, CMake 3.25 or newer, Ninja, pkg-config and a C/C++ compiler
(`setup.sh` lists the packages for Debian / Ubuntu and for macOS). The libraries are linked in, so
the program needs nothing but the system itself: glibc on Linux, the system libraries on macOS.
Internet play checks PSN's certificates against `/etc/ssl/certs/ca-certificates.crt` on Linux and
`/etc/ssl/cert.pem` on macOS.

## Tests

```sh
ctest --test-dir build/release --output-on-failure
```

`tools/smoke_test.py` starts the built program and talks to it like a client: handshake, the
shared clock, discovery, a wake packet, sessions and linking against an address where no console
answers, and a clean exit. It needs no console and no PSN account. Every push is built and tested
this way on Windows, Linux and macOS (`.github/workflows/build.yml`).

## Licence

Nixani Engine is free software under the [GNU Affero General Public License 3.0](LICENSE), like
libchiaki it is built on (AGPL-3.0 with an OpenSSL exception). Whoever gets the program from you
is entitled to its source.

The protocol description and the header `protocol/nxe_protocol.h` are MIT-licensed, so a client
does not have to be AGPL: it is a separate program that only exchanges messages with the engine.

An unofficial project: not affiliated with Sony Interactive Entertainment. PlayStation, PS4, PS5
and DualSense are its trademarks.
