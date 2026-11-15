# Configures and builds nixani-engine inside a Visual Studio x64 developer environment.
# Usage: .\build.ps1 [-Config release|debug]
param([ValidateSet('release', 'debug')] [string]$Config = 'release')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# vcpkg: VCPKG_ROOT if set, else a checkout in .\vcpkg (setup.ps1 makes one).
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $PSScriptRoot 'vcpkg' }
if (-not (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) { throw "vcpkg not found at $env:VCPKG_ROOT. Run .\setup.ps1 or set VCPKG_ROOT." }
$vcpkgRoot = $env:VCPKG_ROOT

if (-not $env:VSCMD_VER) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ Build Tools not found.' }
    $env:Path += ';' + (Split-Path $vswhere)
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    # clang-cl (VS "C++ Clang tools" component): libchiaki uses C99 VLAs, which MSVC's cl.exe rejects.
    $env:Path = (Join-Path $vsPath 'VC\Tools\Llvm\x64\bin') + ';' + $env:Path
}
$env:VCPKG_ROOT = $vcpkgRoot        # the VS developer shell points this at its own vcpkg
$ErrorActionPreference = 'Continue' # native tools report through exit codes

cmake --preset $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build --preset $Config
exit $LASTEXITCODE
