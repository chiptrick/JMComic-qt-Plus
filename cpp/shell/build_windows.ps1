# C1 shell - Windows one-shot build (replaces build_wsl.sh)
#
# Why: the WSL Qt install only has the x86_64 kit (no arm64 libs / no arm64 TLS plugin),
# while the Windows Qt install ships android_arm64_v8a + android_x86_64 complete, and
# NDK / JDK / Gradle are local. Keep this file ASCII-only: Windows PowerShell 5.1 reads
# BOM-less UTF-8 as ANSI and non-ASCII text breaks the parser.
#
# Dependencies (prepared once, not in the repo):
#   <DepsRoot>\libs\<abi>\{libsr_qnn.so,libonnxruntime.so}
#   <DepsRoot>\openssl\<abi>\{libssl.so,libcrypto.so}     # 16KB-aligned preferred
#
# Usage:
#   powershell -File cpp/shell/build_windows.ps1 -Abi arm64-v8a
#   powershell -File cpp/shell/build_windows.ps1 -Abi x86_64 -SkipGates
#
[CmdletBinding()]
param(
    [ValidateSet('arm64-v8a', 'x86_64')] [string]$Abi = 'arm64-v8a',
    [string]$DepsRoot = 'C:\Users\WQ159\jmcomic-build\deps',
    [string]$QtRoot = 'C:\Qt\6.12.0',
    [string]$Ndk = '',
    [string]$ApiLevel = 'android-36',
    [string]$BuildDir = '',
    [switch]$SkipGates
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "[fail] $msg" -ForegroundColor Red; exit 1 }

$C1Dir = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $C1Dir '..\..')).Path
if ($BuildDir) { $Build = $BuildDir } else { $Build = Join-Path $C1Dir "build-win\$Abi" }
$QtAndroid = Join-Path $QtRoot ("android_" + ($Abi -replace '-', '_'))
$QtHost = Join-Path $QtRoot 'mingw_64'
$QtCmake = Join-Path $QtAndroid 'bin\qt-cmake.bat'
# Qt Tools live next to the version dir (C:\Qt\Tools\Ninja), not inside C:\Qt\6.12.0
$QtToolsRoot = Join-Path (Split-Path $QtRoot -Parent) 'Tools'
$Ninja = Join-Path $QtToolsRoot 'Ninja\ninja.exe'
$TlsPlugin = Join-Path $QtAndroid "plugins\tls\libplugins_tls_qopensslbackend_$Abi.so"

$Sdk = $env:ANDROID_HOME
if (-not $Sdk) { $Sdk = $env:ANDROID_SDK_ROOT }
if (-not $Sdk) { $Sdk = 'C:\Users\WQ159\AppData\Local\Android\Sdk' }
if (-not $Ndk) { $Ndk = Join-Path $Sdk 'ndk\30.0.16248370' }

$SslSrc = Join-Path $DepsRoot "openssl\$Abi"
$LibSrc = Join-Path $DepsRoot "libs\$Abi"

# ---- Preflight: fail loudly, never guess ----
foreach ($p in @($QtCmake, $TlsPlugin, (Join-Path $QtHost 'bin\androiddeployqt.exe'),
                 (Join-Path $Ndk 'ndk-build.cmd'), (Join-Path $Sdk "platforms\$ApiLevel"),
                 (Join-Path $SslSrc 'libssl.so'), (Join-Path $SslSrc 'libcrypto.so'),
                 (Join-Path $LibSrc 'libsr_qnn.so'), (Join-Path $C1Dir 'assets\cacert.pem'))) {
    if (-not (Test-Path $p)) { Fail "missing prerequisite: $p" }
}
if (-not (Test-Path $Ninja)) { $Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $Ninja) { Fail "ninja not found (Qt\Tools\Ninja or PATH)" }

# Gradle needs a JVM; prefer Android Studio's JBR over the javapath stub
if (-not $env:JAVA_HOME) {
    $jbr = 'C:\Program Files\Android\Android Studio\jbr'
    if (Test-Path (Join-Path $jbr 'bin\java.exe')) { $env:JAVA_HOME = $jbr }
}
Step "ABI=$Abi Qt=$QtAndroid NDK=$Ndk JAVA_HOME=$env:JAVA_HOME"

# ---- Fresh build dir + OpenSSL staging (with the _3 aliases Qt looks for) ----
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Build | Out-Null
$SslStage = Join-Path $Build 'staging\openssl'
New-Item -ItemType Directory -Force -Path $SslStage | Out-Null
Copy-Item (Join-Path $SslSrc 'libssl.so') (Join-Path $SslStage 'libssl.so') -Force
Copy-Item (Join-Path $SslSrc 'libcrypto.so') (Join-Path $SslStage 'libcrypto.so') -Force
# Qt's OSSL backend on Android asks for libssl_3.so / libcrypto_3.so; a plain
# libssl.so alone yields "Failed to load libssl/libcrypto." at runtime.
Copy-Item (Join-Path $SslSrc 'libssl.so') (Join-Path $SslStage 'libssl_3.so') -Force
Copy-Item (Join-Path $SslSrc 'libcrypto.so') (Join-Path $SslStage 'libcrypto_3.so') -Force

# ---- Configure ----
Step "qt-cmake configure"
& $QtCmake -S $C1Dir -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DANDROID_ABI=$Abi" `
    "-DANDROID_PLATFORM=$ApiLevel" `
    "-DANDROID_SDK_ROOT=$Sdk" `
    "-DANDROID_NDK_ROOT=$Ndk" `
    "-DCMAKE_BUILD_TYPE=Debug" `
    "-DQT_HOST_PATH=$QtHost" `
    "-DSRQ_LIBS_DIR=$LibSrc" `
    "-DOPENSSL_DIR=$SslStage" `
    "-DC1_TLS_PLUGIN=$TlsPlugin"
if ($LASTEXITCODE -ne 0) { Fail "configure failed (exit $LASTEXITCODE)" }

# Wrapper download can hang: pass 1 uses the services.gradle.org URL and pass 2 patches it
# to the Tencent mirror, and the two URLs map to DIFFERENT hash dirs, so pass 1 would
# re-download ~150MB. On a restricted network that download sits at 0 bytes for 30+ min
# (observed: .part stuck at 0). Seed the already-installed gradle into both hash dirs so
# the wrapper hits the local cache instead of downloading.
function Seed-GradleDist {
    $base = Join-Path $env:USERPROFILE '.gradle\wrapper\dists\gradle-9.5.1-bin'
    if (-not (Test-Path $base)) { return }
    $installed = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'gradle-9.5.1\bin\gradle.bat') } |
        Select-Object -First 1
    if (-not $installed) { return }
    # Two fixed hash dirs (gradle uses base36(md5(url)) as the dir name)
    foreach ($h in @('bvvv0hnjcinfezlom7fy9dbyi', 'iq79hdu3mqx29lgffhp8bfmx')) {
        $dst = Join-Path $base $h
        $marker = Join-Path $dst 'gradle-9.5.1\bin\gradle.bat'
        if (Test-Path $marker) { continue }
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item -Recurse -Force (Join-Path $installed.FullName 'gradle-9.5.1') $dst
        New-Item -ItemType File -Force -Path (Join-Path $dst 'gradle-9.5.1-bin.zip.ok') | Out-Null
        Step "gradle dist seeded -> $h"    }
}

Seed-GradleDist

# First pass: generates gradle config. It may fail because Qt defaults compileSdk to 34
# while androidx requires >= 36 - that is expected, we patch and rebuild below.
# NOTE: build output goes to files, never to a pipe. The Gradle daemon inherits the
# child's stdout handle, so piping the build into e.g. `| Select-Object` makes the outer
# pipeline wait forever even after the APK is produced.
# cmake --build on Windows can hang after Gradle succeeds: the daemon (java) inherits
# the stdout handle of androiddeployqt/gradlew, and the direct child exits while the
# daemon keeps the handle alive - `& cmake --build` then never returns (observed 30+ min
# with BUILD SUCCESSFUL already in the log). Workaround: launch cmake detached, poll the
# log for the success marker, then kill the wrapper; the APK is already on disk by then.
function Invoke-BuildPass {
    param([string]$LogFile, [int]$TimeoutSec = 1200)
    # 用 cmd 重定向写日志 + ProcessStartInfo 手工拷环境：
    #   * Start-Process 在 PATH/Path 大小写重复时会抛 "已添加项"(PS 5.1 老毛病)；
    #   * 直接 & cmake 会因 gradle daemon 继承句柄而永远不返回(见下)。
    # 轮询日志里的成功标记，见到就 taskkill /T 清掉包装进程树（APK 已落盘）。
    $cmake = (Get-Command cmake).Source
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = "/c cd /d `"$Build`" && `"$cmake`" --build . > `"$LogFile`" 2>&1"
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    # 不拷贝环境：UseShellExecute=false 时子进程原样继承当前环境，
    # 也就没有 Start-Process 的 PATH/Path 大小写重复问题。
    $p = [System.Diagnostics.Process]::Start($psi)
    $marker = 'Android package built successfully'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Start-Sleep -Seconds 5
        if ((Test-Path $LogFile) -and (Get-Content $LogFile -Raw -ErrorAction SilentlyContinue) -match $marker) {
            Start-Sleep -Seconds 3   # let androiddeployqt finish writing the APK
            if (-not $p.HasExited) {
                & cmd /c "taskkill /T /F /PID $($p.Id)" *> $null
            }
            Step "build pass done (marker seen, wrapper killed)"
            return $true
        }
    }
    if (-not $p.HasExited) {
        & cmd /c "taskkill /T /F /PID $($p.Id)" *> $null
    }
    return $false
}

Step "build pass 1 (generate gradle config)"
$Log1 = Join-Path $Build 'build-pass1.log'
Invoke-BuildPass -LogFile $Log1 | Out-Null
if (Test-Path $Log1) { Get-Content $Log1 -Tail 8 | Out-Host }

$GradleProps = Join-Path $Build 'android-build\gradle.properties'
if (Test-Path $GradleProps) {
    (Get-Content $GradleProps) -replace '^androidCompileSdkVersion=.*', "androidCompileSdkVersion=$ApiLevel" |
        Set-Content $GradleProps -Encoding ascii
    Step "gradle.properties: compileSdk=$ApiLevel"
}

# CA bundle into assets (qrc is the primary path, assets is the fallback + gate check)
$AssetsDir = Join-Path $Build 'android-build\assets'
New-Item -ItemType Directory -Force -Path $AssetsDir | Out-Null
Copy-Item (Join-Path $C1Dir 'assets\cacert.pem') (Join-Path $AssetsDir 'cacert.pem') -Force

# Wrapper via Tencent mirror (upstream often fails TLS through a proxy here)
$WrapperProps = Join-Path $Build 'android-build\gradle\wrapper\gradle-wrapper.properties'
if (Test-Path $WrapperProps) {
    (Get-Content $WrapperProps) -replace '^distributionUrl=.*',
        'distributionUrl=https\://mirrors.cloud.tencent.com/gradle/gradle-9.5.1-bin.zip' |
        Set-Content $WrapperProps -Encoding ascii
    Step "gradle wrapper -> Tencent mirror"
}

Step "build pass 2 (compile + package)"
$Log2 = Join-Path $Build 'build-pass2.log'
Invoke-BuildPass -LogFile $Log2 | Out-Null
if (Test-Path $Log2) { Get-Content $Log2 -Tail 8 | Out-Host }
if ($LASTEXITCODE -ne 0) { Fail "build failed (exit $LASTEXITCODE)" }

# Gradle daemon keeps the caller's stdout handle open: a caller that pipes this script
# (`| Select-Object ...`) would wait forever even though the APK is done. Stop it here.
$Gradlew = Join-Path $Build 'android-build\gradlew.bat'
if (Test-Path $Gradlew) {
    & $Gradlew --stop *> (Join-Path $Build 'gradle-stop.log')
    Step "gradle daemon stopped"
}

$Apk = Join-Path $Build 'android-build\build\outputs\apk\debug\android-build-debug.apk'
if (-not (Test-Path $Apk)) { Fail "no APK produced: $Apk" }
$sizeMb = [math]::Round((Get-Item $Apk).Length / 1MB, 1)
Step "APK: $Apk ($sizeMb MB)"

if (-not $SkipGates) {
    # ---- Gate 1: required entries inside the APK ----
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Apk)
    $names = $zip.Entries | ForEach-Object { $_.FullName }
    $zip.Dispose()
    $required = @(
        'assets/cacert.pem',
        "lib/$Abi/libssl.so",
        "lib/$Abi/libcrypto.so",
        "lib/$Abi/libssl_3.so",
        "lib/$Abi/libcrypto_3.so",
        "lib/$Abi/libplugins_tls_qopensslbackend_$Abi.so"
    )
    foreach ($r in $required) {
        if ($names -contains $r) { Write-Host "[OK] APK contains $r" -ForegroundColor Green }
        else { Fail "APK misses $r (Qt TLS backend will not find OpenSSL)" }
    }

    # ---- Gate 2: direct link (main .so must NEED libsr_qnn.so) ----
    $ReadElf = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
    $AppSo = Get-ChildItem $Build -Filter "libc1shell_*.so" -File | Select-Object -First 1
    if (-not $AppSo) { Fail "libc1shell_*.so not found, cannot verify direct link" }
    $needed = & $ReadElf -d $AppSo.FullName | Select-String 'NEEDED'
    if ($needed -match 'libsr_qnn\.so') {
        Write-Host "[OK] direct link: $($AppSo.Name) DT_NEEDED has libsr_qnn.so" -ForegroundColor Green
    } else {
        Fail "$($AppSo.Name) DT_NEEDED has no libsr_qnn.so - direct link broken"
    }

    # ---- Gate 3: 16KB page alignment ----
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) {
        & $py (Join-Path $Repo 'android\tools\check_elf_page_align.py') $Apk
        if ($LASTEXITCODE -ne 0) { Fail "16KB gate failed" }
    } else {
        Write-Host "[warn] python not found, skip 16KB gate" -ForegroundColor Yellow
    }
}

# ---- Archive ----
$Dist = Join-Path $Repo 'android\dist'
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$Version = '0.2'
$Dest = Join-Path $Dist "JMComic-c1-$Version-$Abi-debug.apk"
Copy-Item $Apk $Dest -Force
Step "archived: $Dest"
