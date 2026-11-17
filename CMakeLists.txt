cmake_minimum_required(VERSION 3.25)

project(nixani-engine VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Builds on Windows (clang-cl), Linux (gcc or clang) and macOS (Apple clang).

# ---------------------------------------------------------------------------
# libchiaki (chiaki-ng): the Remote Play protocol. Its GUI, CLI and decoders are
# not built; the engine only passes the stream on.
# ---------------------------------------------------------------------------
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_RUDP ON CACHE STRING "" FORCE)

# Code generation: protoc from grpcio-tools (see tools/protoc and tools/protoc.cmd) + the nanopb
# generator, both via .venv.
if(WIN32)
	set(NXE_VENV_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
	set(NXE_PROTOC "${CMAKE_CURRENT_SOURCE_DIR}/tools/protoc.cmd")
	set(NXE_SETUP_HINT "Run .\\setup.ps1 (or: python -m venv .venv && .venv\\Scripts\\pip install protobuf grpcio-tools)")
else()
	set(NXE_VENV_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
	set(NXE_PROTOC "${CMAKE_CURRENT_SOURCE_DIR}/tools/protoc")
	set(NXE_SETUP_HINT "Run ./setup.sh (or: python3 -m venv .venv && .venv/bin/pip install protobuf grpcio-tools)")
endif()
if(NOT EXISTS "${NXE_VENV_PYTHON}")
	message(FATAL_ERROR "Python venv missing. ${NXE_SETUP_HINT}")
endif()
set(PROTOC "${NXE_PROTOC}" CACHE FILEPATH "" FORCE)
set(PYTHON_EXECUTABLE "${NXE_VENV_PYTHON}" CACHE FILEPATH "" FORCE)
set(Python_EXECUTABLE "${NXE_VENV_PYTHON}" CACHE FILEPATH "" FORCE)
set(nanopb_BUILD_GENERATOR OFF CACHE BOOL "" FORCE)
set(nanopb_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)

if(NOT WIN32)
	# libchiaki's own copies of nanopb and jerasure, whatever the system has installed.
	set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
	set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
	# The bundled curl (PSN requests for internet play): OpenSSL for TLS and nothing optional, so the
	# engine does not pick up whatever libraries the build machine happens to have. (On Windows it
	# uses Schannel; libchiaki sets that up itself.)
	set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
	set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
	set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
	set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
	set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
	set(CURL_ZLIB OFF CACHE STRING "" FORCE)
	set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
	set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
	# Where the system keeps the certificates of the authorities it trusts. Fixed here instead of
	# taken from the build machine. The engine warns at startup if the file is not there.
	if(APPLE)
		set(NXE_CA_BUNDLE "/etc/ssl/cert.pem")
	else()
		set(NXE_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt")
	endif()
	set(CURL_CA_BUNDLE "${NXE_CA_BUNDLE}" CACHE STRING "" FORCE)
	set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
endif()

add_subdirectory(third_party/chiaki-ng EXCLUDE_FROM_ALL)

if(MSVC)
	# gf-complete calls __cpuidex without including <intrin.h>; clang 16+ rejects the implicit declaration.
	target_compile_options(gf_complete PRIVATE /FIintrin.h)
endif()

# chiaki-lib passes -Wall, which clang-cl treats as -Weverything. Keep its warnings out of the log.
foreach(tgt chiaki-lib gf_complete jerasure protobuf-nanopb-static libcurl_object libcurl_static)
	if(TARGET ${tgt})
		target_compile_options(${tgt} PRIVATE -w)
	endif()
endforeach()

# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------
add_executable(nixani-engine src/main.cpp protocol/nxe_protocol.h)
target_compile_definitions(nixani-engine PRIVATE NXE_VERSION="${PROJECT_VERSION}")
target_link_libraries(nixani-engine PRIVATE chiaki-lib)
if(WIN32)
	target_compile_definitions(nixani-engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
	target_compile_options(nixani-engine PRIVATE /W4 /utf-8)
	target_link_libraries(nixani-engine PRIVATE ws2_32 iphlpapi avrt)
else()
	target_compile_definitions(nixani-engine PRIVATE NXE_CA_BUNDLE="${NXE_CA_BUNDLE}")
	target_compile_options(nixani-engine PRIVATE -Wall -Wextra)
	if(NOT APPLE)
		# One file that runs on any distribution: only glibc is taken from the system.
		target_link_options(nixani-engine PRIVATE -static-libstdc++ -static-libgcc)
	endif()
endif()

# ---------------------------------------------------------------------------
# Tests: no console needed. `ctest --test-dir build/release --output-on-failure`
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME version COMMAND nixani-engine --version)
set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "nixani-engine ${PROJECT_VERSION} \\(protocol [0-9]+\\)")
add_test(NAME protocol COMMAND "${NXE_VENV_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/smoke_test.py" "$<TARGET_FILE:nixani-engine>")
set_tests_properties(protocol PROPERTIES TIMEOUT 180)
