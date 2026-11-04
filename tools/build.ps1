# Builds legacy169.dll (build\Release\legacy169.dll) and the unit tests, then runs the tests.
# PluginSDK: -Sdk DIR, else env LEGACY169_PLUGINSDK_DIR. CMake: the one on PATH, else Visual Studio's bundled cmake.exe.
# -Image FILE: the tests also check the expected bytes against a D2R image dump (file offset == RVA).
param([string]$Sdk, [string]$Image)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (-not $Sdk) { $Sdk = $env:LEGACY169_PLUGINSDK_DIR }
if (-not $Sdk -or -not (Test-Path (Join-Path $Sdk 'CMakeLists.txt'))) { throw 'PluginSDK not found: pass -Sdk DIR or set LEGACY169_PLUGINSDK_DIR' }
$Sdk = (Resolve-Path $Sdk).Path
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $cmake) { throw 'cmake not found (PATH or Visual Studio)' }
}
# temp files of CMake and the compilers stay inside the project
$tmp = Join-Path $root 'work\tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TEMP = $tmp; $env:TMP = $tmp
& $cmake -S . -B build -A x64 "-DLEGACY169_PLUGINSDK_DIR=$($Sdk -replace '\\', '/')"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build build --config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$testArgs = @()
if ($Image) { $testArgs = @((Resolve-Path $Image).Path) }
& (Join-Path $root 'build\tests\Release\legacy169_tests.exe') @testArgs
exit $LASTEXITCODE
