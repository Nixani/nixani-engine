# vcpkg's arm64-osx, plus the oldest macOS the engine runs on. Without this the libraries are built
# for the macOS version of the build machine and the program may not start on an older one.
# (libchiaki sets the same 13.0 for its own code on Apple silicon.)
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.0)
