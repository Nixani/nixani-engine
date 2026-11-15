# Packs the engine into dist\nixani-engine: the exe, the DLLs it needs, the licence texts and where
# the source is. A client program ships that folder as it is.
# Usage: .\package.ps1 [-SkipBuild]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $SkipBuild) {
    & .\build.ps1 -Config release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
$version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(nixani-engine VERSION ([\d.]+)').Groups[1].Value
if (-not $version) { throw 'Could not read the version from CMakeLists.txt.' }

$build = Join-Path $PSScriptRoot 'build\release'
$out = Join-Path $PSScriptRoot 'dist\nixani-engine'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$build\nixani-engine.exe", "$build\*.dll" $out

# Microsoft's VC++ runtime DLLs next to the exe, so the target PC needs no separate runtime install.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem "$vsPath\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'VC++ runtime (VC\Redist) not found in the Visual Studio install.' }
Copy-Item "$($crt.FullName)\*.dll" $out

# Every DLL the binaries import must be in the folder or part of Windows itself.
$dumpbin = Get-ChildItem "$vsPath\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Sort-Object FullName | Select-Object -Last 1
$shipped = Get-ChildItem $out -File -Include *.exe, *.dll -Recurse
$names = $shipped | ForEach-Object { $_.Name.ToLower() }
$missing = @()
foreach ($bin in $shipped) {
    $imports = & $dumpbin.FullName /nologo /dependents $bin.FullName | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim().ToLower() }
    foreach ($dll in $imports) {
        if ($names -contains $dll) { continue }
        $runtime = $dll -match '^(msvcp|vcruntime|concrt|vccorlib|vcomp|ucrtbased|mfc)'
        $windows = $dll -match '^(api-ms-win-|ext-ms-)' -or (Test-Path "$env:SystemRoot\System32\$dll")
        if ($runtime -or -not $windows) { $missing += "$($bin.Name) needs $dll" }
    }
}
if ($missing) { throw "The package would be incomplete:`n  " + (($missing | Sort-Object -Unique) -join "`n  ") }

$notice = @"
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
"@
[IO.File]::WriteAllText("$out\LICENSE.txt", $notice)
Copy-Item 'LICENSE' "$out\LICENSE-AGPL-3.0.txt"

$share = 'vcpkg_installed\x64-windows\share'
$chiaki = 'third_party\chiaki-ng'
$parts = [ordered]@{
    'chiaki-ng (libchiaki)'  = "$chiaki\LICENSES\AGPL-3.0-only-OpenSSL.txt"
    'curl (in libchiaki)'    = "$chiaki\third-party\curl\COPYING"
    'nanopb (in libchiaki)'  = "$chiaki\third-party\nanopb\LICENSE.txt"
    'Jerasure (in libchiaki)' = "$chiaki\third-party\jerasure\License.txt"
    'GF-Complete (in libchiaki)' = "$chiaki\third-party\gf-complete\License.txt"
    'OpenSSL'                = "$share\openssl\copyright"
    'Opus'                   = "$share\opus\copyright"
    'json-c'                 = "$share\json-c\copyright"
    'libevent'               = "$share\libevent\copyright"
    'MiniUPnPc'              = "$share\miniupnpc\copyright"
}
$notices = "Nixani Engine $version includes the components below, each under its own licence.`r`n" +
    "The Microsoft Visual C++ runtime DLLs are redistributed under the Visual Studio licence terms.`r`n"
foreach ($name in $parts.Keys) {
    if (-not (Test-Path $parts[$name])) { throw "Licence text missing for ${name}: $($parts[$name])" }
    $notices += "`r`n`r`n" + ('=' * 78) + "`r`n$name`r`n" + ('=' * 78) + "`r`n`r`n" + [IO.File]::ReadAllText((Resolve-Path $parts[$name])).Trim()
}
[IO.File]::WriteAllText("$out\THIRD-PARTY-NOTICES.txt", $notices)

"Packed $((Get-ChildItem $out -File).Count) files into $out"
