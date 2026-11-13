# C++ UI - Android one-shot build (x86_64 emulator / arm64-v8a device)
#
# Produces a debug-signed APK from the same sources as the Windows desktop build:
#   cpp\shell\ui\build-android-<abi>\  ->  cpp\dist\JMComic-ui-0.89-<abi>-debug.apk
#
# Prerequisites (prepared once):
#   * Qt 6.12 with android_<abi> kit under C:\Qt\6.12.0
#   * Android SDK + NDK (default: %LOCALAPPDATA%\Android\Sdk\ndk\30.0.16248370)
#   * JDK (Android Studio JBR is used automatically when JAVA_HOME is unset)
#   * OpenSSL libs for Android: <DepsRoot>\openssl\<abi>\{libssl.so,libcrypto.so}
#     (same copies the Python APK uses; they are staged with _3 aliases)
#   * For ECH + HTTP/3: <DepsRoot>\openssl41-android\<abi> and
#     <DepsRoot>\quic-android\<abi> (see build_quic_deps_android.sh).
#     When both are present QUIC is enabled automatically; -NoQuic opts out.
#     The produced APK is then checked: libssl.so must export the ECH and
#     QUIC-TLS entry points and the app library must contain the H3 backend.
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less UTF-8 as ANSI.
#
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'arm64-v8a')] [string]$Abi = 'x86_64',
    [string]$QtRoot = 'C:\Qt\6.12.0',
    [string]$Sdk = '',
    [string]$Ndk = '',
    [string]$ApiLevel = 'android-34',
    [string]$DepsRoot = 'C:\Users\WQ159\jmcomic-build\deps',
    [string]$VersionName = '0.89',
    # QUIC/HTTP-3 + ECH: per-ABI deps produced by build_quic_deps_android.sh.
    # Default is AUTO (use them when present); -WithQuic forces ON and fails if
    # they are missing, -NoQuic forces OFF.
    [switch]$WithQuic,
    [switch]$NoQuic,
    [switch]$SkipSign,
    # zipalign.exe hangs on 300MB+ APKs on this host (AV/Defender scan).
    # The gradle-produced APK already installs fine when signed directly.
    [switch]$WithZipAlign,
    [switch]$Clean
)

# Native tools (qt-cmake / ninja / gradle) write warnings to stderr; with 'Stop'
# PS 5.1 turns those into terminating NativeCommandError. Use Continue and check
# $LASTEXITCODE / file existence explicitly instead.
$ErrorActionPreference = 'Continue'
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[fail] $m" -ForegroundColor Red; exit 1 }

# --- APK introspection without external tools -------------------------------
# A zip entry is read as Latin-1 so that one byte == one char: ASCII symbols can
# be searched directly, UTF-16 string literals (QStringLiteral) after encoding
# the needle with [Text.Encoding]::Unicode first.
Add-Type -AssemblyName System.IO.Compression -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Read-ApkEntryText([System.IO.Compression.ZipArchive]$Zip, [string]$Pattern) {
    $e = $Zip.Entries | Where-Object { $_.FullName -like $Pattern } | Select-Object -First 1
    if (-not $e) { return $null }
    $s = $e.Open()
    $ms = New-Object System.IO.MemoryStream
    try { $s.CopyTo($ms) } finally { $s.Dispose() }
    return $Latin1.GetString($ms.ToArray())
}

function Find-Ascii([string]$Hay, [string]$Needle) {
    if (-not $Hay) { return $false }
    return $Hay.IndexOf($Needle, [System.StringComparison]::Ordinal) -ge 0
}

function Find-Utf16([string]$Hay, [string]$Needle) {
    if (-not $Hay) { return $false }
    $w = $Latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($Needle))
    return $Hay.IndexOf($w, [System.StringComparison]::Ordinal) -ge 0
}

$UiDir = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $UiDir '..\..\..')).Path
$Dist = Join-Path $Repo 'cpp\dist'
$Build = Join-Path $UiDir ("build-android-" + $Abi)
$QtAndroid = Join-Path $QtRoot ("android_" + ($Abi -replace '-', '_'))
$QtHost = Join-Path $QtRoot 'mingw_64'
$QtCmake = Join-Path $QtAndroid 'bin\qt-cmake.bat'
$Ninja = Join-Path (Split-Path $QtRoot -Parent) 'Tools\Ninja\ninja.exe'
$TlsPlugin = Join-Path $QtAndroid ("plugins\tls\libplugins_tls_qopensslbackend_" + $Abi + ".so")

if (-not $Sdk) {
    $Sdk = $env:ANDROID_HOME
    if (-not $Sdk) { $Sdk = $env:ANDROID_SDK_ROOT }
    if (-not $Sdk) { $Sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
}
if (-not $Ndk) {
    $NdkCandidates = Get-ChildItem (Join-Path $Sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    if ($NdkCandidates.Count -gt 0) { $Ndk = $NdkCandidates[0].FullName }
}

foreach ($p in @($QtCmake, $TlsPlugin, (Join-Path $QtHost 'bin\androiddeployqt.exe'),
                 (Join-Path $Ndk 'ndk-build.cmd'), (Join-Path $Sdk "platforms\$ApiLevel"))) {
    if (-not (Test-Path $p)) { Fail "missing prerequisite: $p" }
}
if (-not (Test-Path $Ninja)) { $Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $Ninja) { Fail 'ninja not found' }

if (-not $env:JAVA_HOME) {
    $jbr = 'C:\Program Files\Android\Android Studio\jbr'
    if (Test-Path (Join-Path $jbr 'bin\java.exe')) { $env:JAVA_HOME = $jbr }
}
if (-not $env:JAVA_HOME) { Fail 'JAVA_HOME not set and Android Studio JBR not found' }

$SslSrc = Join-Path $DepsRoot "openssl\$Abi"
foreach ($f in @('libssl.so', 'libcrypto.so')) {
    if (-not (Test-Path (Join-Path $SslSrc $f))) { Fail "missing $SslSrc\$f" }
}

# ONNX Runtime for super resolution: same per-ABI deps tree the Python APK used.
# Without it the SR page reports "libonnxruntime.so missing" and the feature is dead,
# so treat it as a hard prerequisite.
$OrtSrc = Join-Path $DepsRoot "libs\$Abi\libonnxruntime.so"
if (-not (Test-Path $OrtSrc)) { Fail "missing $OrtSrc (onnxruntime for $Abi)" }

# OpenSSL 4.x + ngtcp2/nghttp3 (per-ABI): required for ECH and HTTP/3.
# AUTO is the default: when every per-ABI file is present they are used and the
# APK really gets ECH/H3; otherwise the build continues without them and the
# settings page reports "backend not supported" for both switches.
$Ossl4 = Join-Path $DepsRoot "openssl41-android\$Abi"
$QuicDeps = Join-Path $DepsRoot "quic-android\$Abi"
$QuicFiles = @(
    (Join-Path $Ossl4 'lib\libssl.so'),
    (Join-Path $Ossl4 'lib\libcrypto.so'),
    (Join-Path $Ossl4 'include\openssl\ssl.h'),
    (Join-Path $QuicDeps 'lib\libngtcp2.a'),
    (Join-Path $QuicDeps 'lib\libngtcp2_crypto_ossl.a'),
    (Join-Path $QuicDeps 'lib\libnghttp3.a'))
$QuicDepsReady = $true
foreach ($p in $QuicFiles) {
    if (-not (Test-Path $p)) { $QuicDepsReady = $false }
}

if ($WithQuic -and $NoQuic) { Fail '-WithQuic and -NoQuic are mutually exclusive' }
if ($NoQuic) {
    $UseQuic = $false
    Write-Host 'QUIC/ECH: OFF (-NoQuic) - HTTP/3 and ECH stay unavailable' -ForegroundColor Yellow
} elseif ($WithQuic) {
    if (-not $QuicDepsReady) {
        Fail ("QUIC dependency missing under $Ossl4 / $QuicDeps " +
              "(run cpp/shell/quic/tools/build_quic_deps_android.sh $Abi)")
    }
    $UseQuic = $true
} elseif ($QuicDepsReady) {
    $UseQuic = $true
    Write-Host "QUIC/ECH: ON (auto-detected deps in $Ossl4, $QuicDeps)" -ForegroundColor Green
} else {
    $UseQuic = $false
    Write-Host "QUIC/ECH: OFF (per-ABI deps not found in $Ossl4 / $QuicDeps)" -ForegroundColor Yellow
    Write-Host '          HTTP/3 and ECH will NOT be available in this APK.' -ForegroundColor Yellow
}

Step "ABI=$Abi Qt=$QtAndroid NDK=$Ndk JAVA_HOME=$env:JAVA_HOME"
if ($Clean -and (Test-Path $Build)) { Remove-Item $Build -Recurse -Force }

# --- OpenSSL staging: Qt's TLS backend dlopens libssl_3/libcrypto_3 on Android ---
$Stage = Join-Path $Build 'staging'
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
Copy-Item (Join-Path $SslSrc 'libssl.so') (Join-Path $Stage 'libssl_3.so') -Force
Copy-Item (Join-Path $SslSrc 'libcrypto.so') (Join-Path $Stage 'libcrypto_3.so') -Force
# Qt's TLS plugin dlopens libssl_3.so / libcrypto_3.so on Android; those two
# aliases are enough. The unversioned libssl.so / libcrypto.so must not exist
# twice: with QUIC they belong to OpenSSL 4.x (QUIC-TLS + ECH), without QUIC
# they stay OpenSSL 3.x and keep the previous behaviour.
if (-not $UseQuic) {
    Copy-Item (Join-Path $SslSrc 'libssl.so') (Join-Path $Stage 'libssl.so') -Force
    Copy-Item (Join-Path $SslSrc 'libcrypto.so') (Join-Path $Stage 'libcrypto.so') -Force
}

$ExtraList = @(
    (Join-Path $Stage 'libssl_3.so'),
    (Join-Path $Stage 'libcrypto_3.so'),
    $TlsPlugin
)
if (-not $UseQuic) {
    $ExtraList += (Join-Path $Stage 'libssl.so')
    $ExtraList += (Join-Path $Stage 'libcrypto.so')
} else {
    # CMake also appends the two 4.x .so files to UI_ANDROID_EXTRA_LIBS; adding
    # them here keeps the -DUI_ANDROID_EXTRA_LIBS value self-contained.
    $ExtraList += (Join-Path $Ossl4 'lib\libssl.so')
    $ExtraList += (Join-Path $Ossl4 'lib\libcrypto.so')
}
$Extra = $ExtraList -join ';'

Step 'configure'
# QUIC (OpenSSL) keeps its own trust store: Android has no default CA file and
# some images cannot load /apex/.../cacerts at all (verify=20 during the QUIC
# handshake). CMake compiles <build>\generated\cacert.pem into :/tls/cacert.pem,
# so stage the host CA bundle there before configuring - same as the .sh flow.
$CaSrc = @('C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt',
           'C:\Program Files\Git\mingw64\ssl\certs\ca-bundle.crt') |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if ($CaSrc) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Build 'generated') | Out-Null
    Copy-Item $CaSrc (Join-Path $Build 'generated\cacert.pem') -Force
    Write-Host "CA bundle: $CaSrc -> build\generated\cacert.pem"
} else {
    Write-Host 'CA bundle not found - QUIC/HTTP3 will fail certificate verify (verify=20)' -ForegroundColor Yellow
}
& $QtCmake -S $UiDir -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DANDROID_ABI=$Abi" `
    "-DANDROID_PLATFORM=$ApiLevel" `
    "-DANDROID_SDK_ROOT=$Sdk" `
    "-DANDROID_NDK_ROOT=$Ndk" `
    '-DCMAKE_BUILD_TYPE=Release' `
    "-DQT_HOST_PATH=$QtHost" `
    "-DUI_ANDROID_EXTRA_LIBS=$Extra" `
    "-DJM_ORT_ANDROID_DIR=$(Join-Path $DepsRoot "libs\$Abi")" `
    "-DJM_WITH_QUIC=$([int]$UseQuic)" `
    "-DJM_QUIC_ROOT=$QuicDeps" `
    "-DJM_OPENSSL41_ROOT=$Ossl4"
if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" }

# --- Build: gradle daemon may hold stdout, so redirect to a log and check the tail ---
Step 'build + package (ninja -> androiddeployqt -> gradle)'
$Log = Join-Path $Build 'build.log'
& cmd /c "`"$Ninja`" -C `"$Build`" > `"$Log`" 2>&1"
if ($LASTEXITCODE -ne 0) { Get-Content $Log -Tail 25 | Out-Host; Fail 'build failed (see build.log)' }
Get-Content $Log -Tail 4 | Out-Host

$Unsigned = Join-Path $Build 'android-build\build\outputs\apk\release\android-build-release-unsigned.apk'
if (-not (Test-Path $Unsigned)) { Fail "no APK produced: $Unsigned" }

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$OutApk = Join-Path $Dist ("JMComic-ui-" + $VersionName + "-" + $Abi + "-debug.apk")

if ($SkipSign) {
    Copy-Item $Unsigned $OutApk -Force
} else {
    # String sort puts 9.0.0 before 36.0.0; sort by [version] and keep only the
    # build-tools release that actually ships zipalign + apksigner.
    $Bt = Get-ChildItem (Join-Path $Sdk 'build-tools') -Directory |
        Where-Object {
            (Test-Path (Join-Path $_.FullName 'zipalign.exe')) -and
            (Test-Path (Join-Path $_.FullName 'apksigner.bat'))
        } |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0.0' } } -Descending |
        Select-Object -First 1
    if (-not $Bt) { Fail 'no usable build-tools (zipalign/apksigner) found' }
    $ApkSigner = Join-Path $Bt.FullName 'apksigner.bat'
    $Ks = Join-Path $env:USERPROFILE '.android\debug.keystore'
    if (-not (Test-Path $Ks)) { Fail "debug keystore not found: $Ks" }
    $signInput = $Unsigned
    if ($WithZipAlign) {
        $ZipAlign = Join-Path $Bt.FullName 'zipalign.exe'
        $Aligned = Join-Path $Build 'aligned.apk'
        & $ZipAlign -f 4 $Unsigned $Aligned
        if ($LASTEXITCODE -ne 0) { Fail 'zipalign failed' }
        $signInput = $Aligned
    } else {
        Write-Host '  note  skipping zipalign (use -WithZipAlign to force it)' -ForegroundColor Yellow
    }
    & $ApkSigner sign --ks $Ks --ks-pass pass:android --ks-key-alias androiddebugkey `
        --key-pass pass:android --out $OutApk $signInput 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $OutApk)) { Fail 'apksigner failed' }
}

$sizeMb = [math]::Round((Get-Item $OutApk).Length / 1MB, 1)
Step "APK: $OutApk ($sizeMb MB)"

# --- ECH / HTTP3 gate: a build that lacks the symbols is a build without the
# features (runtime would silently report ech=0 / http3=0), so fail here.
Step 'verify ECH/HTTP3 inside the APK'
$hasEch = $false
$hasQuicTls = $false
$hasH3Backend = $false
$zip = [System.IO.Compression.ZipFile]::OpenRead($OutApk)
try {
    $libDir = 'lib/' + $Abi + '/'
    $sslText = Read-ApkEntryText $zip ($libDir + 'libssl.so')
    $uiText = Read-ApkEntryText $zip ($libDir + 'libjmcomic_ui*.so')
    if (-not $sslText -or -not $uiText) {
        Fail "APK verification: expected native libs under $libDir were not found"
    }
    $hasEch = Find-Ascii $sslText 'SSL_set1_ech_config_list'
    $hasQuicTls = Find-Ascii $sslText 'SSL_set_quic_tls_cbs'
    # This literal only exists in the JM_WITH_QUIC build of BackendSummary();
    # the stub branch of quic_http.cpp has a different string.
    $hasH3Backend = Find-Utf16 $uiText 'ngtcp2 %1 / nghttp3 %2 / %3'
} finally {
    $zip.Dispose()
}

if ($hasEch) { Write-Host '  ok    libssl.so exports SSL_set1_ech_config_list' -ForegroundColor Green }
else         { Write-Host '  MISS  libssl.so has no ECH export' -ForegroundColor Red }
if ($hasQuicTls) { Write-Host '  ok    libssl.so exports SSL_set_quic_tls_cbs' -ForegroundColor Green }
else             { Write-Host '  MISS  libssl.so has no QUIC-TLS callback API' -ForegroundColor Red }
if ($hasH3Backend) { Write-Host '  ok    app library contains the ngtcp2/nghttp3 backend' -ForegroundColor Green }
else               { Write-Host '  MISS  app library was built with the QUIC stub' -ForegroundColor Red }

if ($UseQuic -and (-not ($hasEch -and $hasQuicTls -and $hasH3Backend))) {
    Fail ('QUIC/ECH verification failed: this APK would report ech=0 / http3=0. ' +
          'Re-run cpp/shell/quic/tools/build_quic_deps_android.sh ' + $Abi + ' and rebuild.')
}
if (-not $UseQuic) {
    Write-Host '  note  QUIC/ECH intentionally disabled for this build (-NoQuic or deps absent)' -ForegroundColor Yellow
}

# --- SR gate: the feature is "unusable" if the APK lacks the runtime or models.
# A silent miss here is exactly what shipping a dead super-resolution page looks like, so
# fail the build instead of producing an APK that only shows an error string.
Step 'verify super-resolution runtime + models inside the APK'
$hasOrt = $false
$modelCount = 0
$zip = [System.IO.Compression.ZipFile]::OpenRead($OutApk)
try {
    $libDir = 'lib/' + $Abi + '/'
    $hasOrt = ($zip.Entries | Where-Object { $_.FullName -eq ($libDir + 'libonnxruntime.so') }).Count -gt 0
    $modelCount = ($zip.Entries | Where-Object { $_.FullName -like 'assets/sr_models/*.onnx' }).Count
} finally {
    $zip.Dispose()
}
if ($hasOrt) { Write-Host "  ok    $libDir/libonnxruntime.so present" -ForegroundColor Green }
else         { Write-Host "  MISS  $libDir/libonnxruntime.so" -ForegroundColor Red }
if ($modelCount -gt 0) { Write-Host "  ok    $modelCount super-resolution models under assets/sr_models" -ForegroundColor Green }
else                   { Write-Host '  MISS  assets/sr_models is empty' -ForegroundColor Red }
if ((-not $hasOrt) -or ($modelCount -lt 20)) {
    Fail "SR verification failed: onnxruntime=$hasOrt models=$modelCount (need >=20)"
}
