param(
    [Parameter(Mandatory=$true)][string]$ObsSourceDir,
    [string]$ObsInstallDir = 'C:\Program Files (x86)\obs-studio',
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg"
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$obsDll = Join-Path $ObsInstallDir 'bin\64bit\obs.dll'
if (!(Test-Path -LiteralPath $obsDll)) { throw "OBS DLL not found: $obsDll" }
if (!(Test-Path -LiteralPath (Join-Path $ObsSourceDir 'libobs\obs-module.h'))) {
    throw "OBS source headers not found: $ObsSourceDir"
}
$versions = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Directory |
    Sort-Object Name -Descending
if (!$versions) { throw 'MSVC Build Tools are required' }
$tools = Join-Path $versions[0].FullName 'bin\Hostx64\x64'
$dumpbin = Join-Path $tools 'dumpbin.exe'
$lib = Join-Path $tools 'lib.exe'
$sdk = Join-Path $repo 'work\obs-sdk'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
$def = Join-Path $sdk 'obs.def'
$import = Join-Path $sdk 'obs.lib'
$exports = & $dumpbin /exports $obsDll
if ($LASTEXITCODE -ne 0) { throw 'dumpbin failed' }
$names = @($exports | ForEach-Object {
    if ($_ -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] }
})
if ($names.Count -lt 100) { throw "Only $($names.Count) OBS exports found" }
@('LIBRARY obs.dll', 'EXPORTS') + $names | Set-Content -LiteralPath $def -Encoding ASCII
& $lib "/def:$def" "/out:$import" /machine:x64 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'lib.exe failed to create obs.lib' }

$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (!(Test-Path -LiteralPath $toolchain)) { throw "vcpkg toolchain not found: $toolchain" }
$buildScript = Join-Path $PSScriptRoot 'build-obs-native.bat'
& $buildScript $ObsSourceDir $import $VcpkgRoot
if ($LASTEXITCODE -ne 0) { throw 'OBS plugin build failed' }
