# Build the QUIC dependencies for the Windows desktop shell from scratch.
#
#   ngtcp2 1.26.90 + nghttp3 1.19.90, linked against OpenSSL 4.1 (QUIC-TLS + ECH)
#
# Why this script exists (T1): the current C:/Users/WQ159/jmcomic-build/deps tree
# was produced by hand. Without a script nobody can rebuild it, so "works on my
# machine" is the only guarantee. This script reproduces the exact same layout:
#
#   <Prefix>/lib/libnghttp3.a
#   <Prefix>/lib/libngtcp2.a
#   <Prefix>/lib/libngtcp2_crypto_ossl.a
#   <Prefix>/include/nghttp3/*.h  ngtcp2/*.h
#
# Prerequisites:
#   * Qt 6.12 MinGW toolchain (gcc, gendef, dlltool) - C:\Qt\Tools\mingw1310_64
#   * CMake + Ninja                               - C:\Qt\Tools\{CMake_64,Ninja}
#   * OpenSSL 4.1 Windows x64 build (DLLs only is fine; import libs are generated)
#   * nghttp3 / ngtcp2 sources
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build_quic_deps.ps1
#   powershell ... -File build_quic_deps.ps1 -Prefix D:\deps\quic -Avx2
#
# -Avx2: only for desktop x86_64. Do NOT use it for Android or arm64 builds -
# the x86_64 emulator does not necessarily expose AVX2 and arm64 has no such flag.
#
# Keep this file ASCII-only: PowerShell 5.1 reads BOM-less UTF-8 as ANSI.

[CmdletBinding()]
param(
    [string]$OpenSslRoot = 'C:\Users\WQ159\jmcomic-build\deps\openssl41-mingw',
    [string]$Prefix      = 'C:\Users\WQ159\jmcomic-build\deps\quic-mingw',
    [string]$Nghttp3Src  = 'C:\Users\WQ159\nghttp3',
    [string]$Ngtcp2Src   = 'C:\Users\WQ159\ngtcp2',
    [string]$MingwBin    = 'C:\Qt\Tools\mingw1310_64\bin',
    [string]$CMake       = 'C:\Qt\Tools\CMake_64\bin\cmake.exe',
    [string]$Ninja       = 'C:\Qt\Tools\Ninja\ninja.exe',
    [switch]$Avx2,
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'   # native tools write warnings to stderr
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

$Work = Join-Path $env:TEMP 'jm_quic_deps_build'
$Logs = Join-Path $Work 'logs'

Step "environment"
foreach ($p in @($CMake, $Ninja, (Join-Path $MingwBin 'gcc.exe'),
                 (Join-Path $MingwBin 'gendef.exe'), (Join-Path $MingwBin 'dlltool.exe'))) {
    if (-not (Test-Path $p)) { Fail "missing tool: $p" }
}
foreach ($p in @($Nghttp3Src, $Ngtcp2Src)) {
    if (-not (Test-Path (Join-Path $p 'CMakeLists.txt'))) { Fail "not a source tree: $p" }
}
$sslDll = Join-Path $OpenSslRoot 'bin\libssl-4-x64.dll'
$cryDll = Join-Path $OpenSslRoot 'bin\libcrypto-4-x64.dll'
if (-not (Test-Path $sslDll)) { Fail "missing $sslDll" }
if (-not (Test-Path $cryDll)) { Fail "missing $cryDll" }

if ($Clean -and (Test-Path $Work)) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
$env:PATH = "$MingwBin;$env:PATH"

# ---------------------------------------------------------------------------
# 1. OpenSSL DLL -> import libraries (gendef + dlltool)
#    OpenSSL 4.1 ships DLLs but no .lib/.a, and ngtcp2 needs linkable symbols.
# ---------------------------------------------------------------------------
Step "OpenSSL import libraries"
$sslLibDir = Join-Path $OpenSslRoot 'lib'
New-Item -ItemType Directory -Force -Path $sslLibDir | Out-Null
# gendef reads the DLL from its own working directory, so stage both DLLs there first.
Copy-Item $sslDll (Join-Path $sslLibDir 'libssl-4-x64.dll') -Force
Copy-Item $cryDll (Join-Path $sslLibDir 'libcrypto-4-x64.dll') -Force
foreach ($pair in @(@('libssl-4-x64', 'libssl.dll.a'), @('libcrypto-4-x64', 'libcrypto.dll.a'))) {
    $dllName = $pair[0]
    $impName = $pair[1]
    $def = Join-Path $sslLibDir ($dllName + '.def')
    $imp = Join-Path $sslLibDir $impName
    if ((Test-Path $def) -and (Test-Path $imp)) {
        Write-Host "    reuse $imp"
        continue
    }
    Push-Location $sslLibDir
    & (Join-Path $MingwBin 'gendef.exe') ($dllName + '.dll')
    if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "gendef failed for $dllName" }
    & (Join-Path $MingwBin 'dlltool.exe') -d ($dllName + '.def') -l $impName -k
    if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "dlltool failed for $dllName" }
    Pop-Location
}
if (-not (Test-Path (Join-Path $sslLibDir 'libssl.dll.a'))) { Fail 'libssl.dll.a not produced' }
if (-not (Test-Path (Join-Path $sslLibDir 'libcrypto.dll.a'))) { Fail 'libcrypto.dll.a not produced' }

# ---------------------------------------------------------------------------
# 2. nghttp3 (static, library only)
# ---------------------------------------------------------------------------
Step "nghttp3"
$h3Build = Join-Path $Work 'nghttp3'
$h3Args = @(
    '-S', $Nghttp3Src, '-B', $h3Build, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DCMAKE_INSTALL_PREFIX=$Prefix",
    '-DENABLE_LIB_ONLY=ON',
    '-DBUILD_SHARED_LIBS=OFF',
    "-DCMAKE_C_COMPILER=$MingwBin\gcc.exe"
)
if ($Avx2) { $h3Args += '-DENABLE_AVX2=ON' }
& $CMake @h3Args *> (Join-Path $Logs 'nghttp3-configure.log')
if ($LASTEXITCODE -ne 0) { Fail 'nghttp3 configure failed (see logs)' }
& $CMake --build $h3Build --target install *> (Join-Path $Logs 'nghttp3-build.log')
if ($LASTEXITCODE -ne 0) { Fail 'nghttp3 build failed (see logs)' }

# ---------------------------------------------------------------------------
# 3. ngtcp2 (+ crypto helper bound to OpenSSL)
# ---------------------------------------------------------------------------
Step "ngtcp2"
$t2Build = Join-Path $Work 'ngtcp2'
$t2Args = @(
    '-S', $Ngtcp2Src, '-B', $t2Build, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DCMAKE_INSTALL_PREFIX=$Prefix",
    '-DENABLE_LIB_ONLY=ON',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DENABLE_OPENSSL=ON',
    "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
    "-DOPENSSL_INCLUDE_DIR=$OpenSslRoot\include",
    "-DOPENSSL_SSL_LIBRARY=$sslLibDir\libssl.dll.a",
    "-DOPENSSL_CRYPTO_LIBRARY=$sslLibDir\libcrypto.dll.a",
    "-DCMAKE_C_COMPILER=$MingwBin\gcc.exe"
)
$t2Log = Join-Path $Logs 'ngtcp2-configure.log'
& $CMake @t2Args *> $t2Log
if ($LASTEXITCODE -ne 0) { Fail 'ngtcp2 configure failed (see logs)' }

# Validation #1: the OpenSSL build must expose the QUIC-TLS callback API.
# Without this ngtcp2_crypto_ossl compiles but the handshake cannot work.
if (-not (Select-String -Path $t2Log -Pattern 'Looking for SSL_set_quic_tls_cbs - found' -Quiet)) {
    Fail 'OpenSSL has no SSL_set_quic_tls_cbs (not a QUIC-TLS build)'
}

& $CMake --build $t2Build --target install *> (Join-Path $Logs 'ngtcp2-build.log')
if ($LASTEXITCODE -ne 0) { Fail 'ngtcp2 build failed (see logs)' }

# ---------------------------------------------------------------------------
# 4. Post-build checks: files, architecture, required symbols
# ---------------------------------------------------------------------------
Step "verify"
$required = @('libnghttp3.a', 'libngtcp2.a', 'libngtcp2_crypto_ossl.a')
foreach ($lib in $required) {
    $p = Join-Path $Prefix ('lib\' + $lib)
    if (-not (Test-Path $p)) { Fail "missing $p" }
    Write-Host ("    {0}  {1:N0} KB" -f $lib, ((Get-Item $p).Length / 1KB))
}
$nm = Join-Path $MingwBin 'nm.exe'
$sym = & $nm -g (Join-Path $Prefix 'lib\libngtcp2.a') 2>$null | Select-String -Pattern 'ngtcp2_conn_client_new' -Quiet
if (-not $sym) { Fail 'libngtcp2.a has no ngtcp2_conn_client_new' }
$sym = & $nm -g (Join-Path $Prefix 'lib\libngtcp2_crypto_ossl.a') 2>$null | Select-String -Pattern 'ngtcp2_crypto_ossl_init' -Quiet
if (-not $sym) { Fail 'libngtcp2_crypto_ossl.a has no ngtcp2_crypto_ossl_init' }

# ---------------------------------------------------------------------------
# 5. Provenance: versions + checksums (append to docs/deps_quic_versions.txt)
# ---------------------------------------------------------------------------
Step "record versions"
$sha = @{}
foreach ($lib in $required) {
    $sha[$lib] = (Get-FileHash (Join-Path $Prefix ('lib\' + $lib)) -Algorithm SHA256).Hash
}
$line = "built={0:yyyy-MM-dd HH:mm} avx2={1} openssl={2} nghttp3={3} ngtcp2={4}" -f `
    (Get-Date), [int]$Avx2.IsPresent, $OpenSslRoot, $Nghttp3Src, $Ngtcp2Src
Write-Host "    $line"
foreach ($k in $sha.Keys) { Write-Host ("    {0} sha256={1}" -f $k, $sha[$k]) }

Write-Host ""
Write-Host "[ok] QUIC deps installed to $Prefix" -ForegroundColor Green
Write-Host "     Reconfigure the UI build with:" -ForegroundColor Green
Write-Host "     -DJM_WITH_QUIC=ON -DJM_QUIC_ROOT=$Prefix -DJM_OPENSSL41_ROOT=$OpenSslRoot"
