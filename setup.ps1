# One-time setup: submodules, the Python venv used for code generation, and vcpkg.
# Needs: git, Python 3, Visual Studio 2022 Build Tools with the C++ and "C++ Clang tools" components.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

git submodule update --init --depth 1
git -C third_party/chiaki-ng submodule update --init --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl

if (-not (Test-Path .venv\Scripts\python.exe)) { python -m venv .venv }
.venv\Scripts\python.exe -m pip install --quiet --disable-pip-version-check protobuf grpcio-tools

if (-not $env:VCPKG_ROOT -and -not (Test-Path vcpkg\vcpkg.exe)) {
    if (-not (Test-Path vcpkg)) { git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg }
    cmd /c 'vcpkg\bootstrap-vcpkg.bat -disableMetrics'
}
'Ready. Build with .\build.ps1'
