# Builds legacy169.dll (build\Release\legacy169.dll) and the unit tests, then runs the tests.
# PluginSDK: -Sdk DIR, else env LEGACY169_PLUGINSDK_DIR. CMake: the one on PATH, else Visual Studio's bundled cmake.exe.
# -Image FILE: the tests also check the expected bytes against a D2R image dump (file offset == RVA).
# -Test: a test build with the background-input harness (src/testharness.cpp) in build-test\Release\legacy169.dll. Never
# ship it; the release build (no -Test) does not contain the harness.
# -BadBytes: a test build (with the harness) in build-bad\ whose last patch expects a wrong byte: it must refuse everything.
param([string]$Sdk, [string]$Image, [switch]$Test, [switch]$BadBytes)
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
$out = if ($BadBytes) { 'build-bad' } elseif ($Test) { 'build-test' } else { 'build' }
$harness = if ($Test -or $BadBytes) { 'ON' } else { 'OFF' }
$bad = if ($BadBytes) { 'ON' } else { 'OFF' }
& $cmake -S . -B $out -A x64 "-DLEGACY169_PLUGINSDK_DIR=$($Sdk -replace '\\', '/')" "-DLEGACY169_TEST_HARNESS=$harness" "-DLEGACY169_TEST_BAD_BYTES=$bad"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $out --config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$testArgs = @()
if ($Image) { $testArgs = @((Resolve-Path $Image).Path) }
& (Join-Path $root "$out\tests\Release\legacy169_tests.exe") @testArgs
exit $LASTEXITCODE
