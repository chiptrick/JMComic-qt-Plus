# ---------------------------------------------------------------------------
# C++ desktop build - Windows (MinGW-w64 + Qt 6.12 + Ninja)
#
# Output: cpp\shell\ui\build\jmcomic_ui.exe   (default build dir)
#         cpp\dist\windows\...                (with -Deploy, windeployqt)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1
#   powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1 -Deploy
#   powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1 -Clean -Jobs 8
#
# Android packaging is separate (device shell + UI APK):
#   cpp\shell\build_windows.ps1 -Abi x86_64        # device shell APK (sr_qnn/OpenSSL)
#   cpp\shell\ui\build_android.ps1 -Abi x86_64     # UI APK (x86_64 emulator / arm64 device)
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less UTF-8 as ANSI.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$QtRoot = 'C:\Qt\6.12.0',
    [string]$BuildDir = '',
    [int]$Jobs = 0,
    [switch]$Deploy,
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

$CppDir = $PSScriptRoot
$UiDir = Join-Path $CppDir 'shell\ui'
$QtHost = Join-Path $QtRoot 'mingw_64'
$QtTools = Join-Path (Split-Path $QtRoot -Parent) 'Tools'
$CMake = Join-Path $QtTools 'CMake_64\bin\cmake.exe'
$Ninja = Join-Path $QtTools 'Ninja\ninja.exe'
$MinGW = Join-Path $QtTools 'mingw1310_64'
if (-not $BuildDir) { $BuildDir = Join-Path $UiDir 'build' }

foreach ($p in @(
    (Join-Path $QtHost 'bin\Qt6Core.dll'),
    (Join-Path $MinGW 'bin\g++.exe'),
    $CMake,
    $Ninja,
    (Join-Path $UiDir 'CMakeLists.txt')
)) {
    if (-not (Test-Path $p)) { Fail "missing prerequisite: $p" }
}

Step "Qt=$QtHost"
Step "MinGW=$MinGW"
Step "Build=$BuildDir"

if ($Clean -and (Test-Path $BuildDir)) {
    Step 'clean build dir'
    Remove-Item $BuildDir -Recurse -Force
}

& $CMake -S $UiDir -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_PREFIX_PATH=$QtHost" `
    "-DCMAKE_C_COMPILER=$MinGW\bin\gcc.exe" `
    "-DCMAKE_CXX_COMPILER=$MinGW\bin\g++.exe" `
    "-DCMAKE_MAKE_PROGRAM=$Ninja"
if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" }

$buildArgs = @('--build', $BuildDir)
if ($Jobs -gt 0) { $buildArgs += @('--parallel', "$Jobs") }
& $CMake @buildArgs
if ($LASTEXITCODE -ne 0) { Fail "build failed ($LASTEXITCODE)" }

$Exe = Join-Path $BuildDir 'jmcomic_ui.exe'
if (-not (Test-Path $Exe)) { Fail "executable not found: $Exe" }
Step "OK: $Exe"

if ($Deploy) {
    $Dist = Join-Path $CppDir 'dist\windows'
    if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Dist | Out-Null
    Copy-Item $Exe $Dist -Force
    $WinDeploy = Join-Path $QtHost 'bin\windeployqt.exe'
    if (-not (Test-Path $WinDeploy)) { Fail "windeployqt not found: $WinDeploy" }
    & $WinDeploy --no-translations --no-system-d3d-compiler --no-opengl-sw `
        (Join-Path $Dist 'jmcomic_ui.exe')
    if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed ($LASTEXITCODE)" }
    Step "deployed: $Dist"
}
