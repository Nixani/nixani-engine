cmake_minimum_required(VERSION 3.25)

project(nixani-engine VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
	message(FATAL_ERROR "nixani-engine currently builds for Windows only.")
endif()

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

# Code generation: protoc from grpcio-tools (see tools/protoc.cmd) + the nanopb generator, both via .venv.
set(NXE_VENV_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
if(NOT EXISTS "${NXE_VENV_PYTHON}")
	message(FATAL_ERROR "Python venv missing. Run .\\setup.ps1 (or: python -m venv .venv && .venv\\Scripts\\pip install protobuf grpcio-tools)")
endif()
set(PROTOC "${CMAKE_CURRENT_SOURCE_DIR}/tools/protoc.cmd" CACHE FILEPATH "" FORCE)
set(PYTHON_EXECUTABLE "${NXE_VENV_PYTHON}" CACHE FILEPATH "" FORCE)
set(Python_EXECUTABLE "${NXE_VENV_PYTHON}" CACHE FILEPATH "" FORCE)
set(nanopb_BUILD_GENERATOR OFF CACHE BOOL "" FORCE)
set(nanopb_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/chiaki-ng EXCLUDE_FROM_ALL)

# A fix to libchiaki, kept here because chiaki-ng is an unmodified submodule: packetstats.c works out
# how many audio packets went missing as "newest 16-bit frame number minus oldest". C does that
# subtraction in int, so each time the frame number wraps around (every 65536 audio frames = 10 min
# 55 s of streaming) the result is a huge number instead of a small one, and the console is told once
# that about three quarters of all packets were lost. Doing the subtraction in 16 bits gives what the
# code's own comment asks for. The patched copy is compiled in place of the original.
set(NXE_PACKETSTATS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chiaki-ng/lib/src/packetstats.c")
file(READ "${NXE_PACKETSTATS}" NXE_PACKETSTATS_SRC)
string(REPLACE "uint64_t seq_diff = stats->seq_max - stats->seq_min;" "uint64_t seq_diff = (uint16_t)(stats->seq_max - stats->seq_min);"
	NXE_PACKETSTATS_FIXED "${NXE_PACKETSTATS_SRC}")
if(NXE_PACKETSTATS_FIXED STREQUAL NXE_PACKETSTATS_SRC)
	message(FATAL_ERROR "packetstats.c in chiaki-ng no longer has the line this fix replaces: check whether it is still needed.")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/chiaki-fixes/packetstats.c" CONTENT "${NXE_PACKETSTATS_FIXED}" @ONLY) # rewritten only when it changes
get_target_property(NXE_CHIAKI_SOURCES chiaki-lib SOURCES)
list(FILTER NXE_CHIAKI_SOURCES EXCLUDE REGEX "packetstats\\.c$")
set_target_properties(chiaki-lib PROPERTIES SOURCES "${NXE_CHIAKI_SOURCES}")
target_sources(chiaki-lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/chiaki-fixes/packetstats.c")

# gf-complete calls __cpuidex without including <intrin.h>; clang 16+ rejects the implicit declaration.
target_compile_options(gf_complete PRIVATE /FIintrin.h)

# chiaki-lib passes -Wall, which clang-cl treats as -Weverything. Keep its warnings out of the log.
foreach(tgt chiaki-lib gf_complete jerasure protobuf-nanopb-static libcurl_object)
	if(TARGET ${tgt})
		target_compile_options(${tgt} PRIVATE -w)
	endif()
endforeach()

# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------
add_executable(nixani-engine src/main.cpp protocol/nxe_protocol.h)
target_compile_definitions(nixani-engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN NXE_VERSION="${PROJECT_VERSION}")
target_compile_options(nixani-engine PRIVATE /W4 /utf-8)
target_link_libraries(nixani-engine PRIVATE chiaki-lib ws2_32 iphlpapi avrt)
