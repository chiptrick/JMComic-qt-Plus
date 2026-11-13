@echo off
rem ---------------------------------------------------------------------------
rem Build the C++ UI (Windows / MinGW-w64 + Qt 6.12 + Ninja)
rem   build_win.bat            configure + build
rem   build_win.bat clean      wipe build dir first
rem   build_win.bat deploy     build + windeployqt into .\dist
rem NOTE: keep this file ASCII-only. cmd.exe parses .bat in the OEM codepage,
rem       UTF-8 Chinese comments break line parsing on zh-CN Windows.
rem ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
set "QTDIR=C:\Qt\6.12.0\mingw_64"
set "MINGW=C:\Qt\Tools\mingw1310_64"
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "NINJA=C:\Qt\Tools\Ninja\ninja.exe"
set "BUILD=%HERE%build"

rem ---------------------------------------------------------------------------
rem QUIC / OpenSSL 4.x paths are overridable from the environment so the same
rem script works on a machine without the default build tree.
rem   set JM_WITH_QUIC=OFF   -> link without ngtcp2/nghttp3 (no runtime DLL copy)
rem ---------------------------------------------------------------------------
if not defined JM_WITH_QUIC set "JM_WITH_QUIC=ON"
if not defined JM_QUIC_ROOT set "JM_QUIC_ROOT=C:\Users\WQ159\jmcomic-build\deps\quic-mingw"
if not defined JM_OPENSSL41_ROOT set "JM_OPENSSL41_ROOT=C:\Users\WQ159\jmcomic-build\deps\openssl41-mingw"

if not exist "%QTDIR%\bin\Qt6Core.dll" (
    echo [x] Qt not found: %QTDIR%
    echo     Edit QTDIR / MINGW at the top of this script if installed elsewhere.
    exit /b 2
)

if /i "%1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

"%CMAKE%" -S "%HERE%." -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=%QTDIR% ^
  -DCMAKE_C_COMPILER=%MINGW%/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=%MINGW%/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=%NINJA% ^
  -DJM_WITH_QUIC=%JM_WITH_QUIC% ^
  -DJM_QUIC_ROOT=%JM_QUIC_ROOT% ^
  -DJM_OPENSSL41_ROOT=%JM_OPENSSL41_ROOT%
if errorlevel 1 exit /b 1

"%NINJA%" -C "%BUILD%"
if errorlevel 1 exit /b 1
echo [ok] %BUILD%\jmcomic_ui.exe

rem NOTE: DIST must be defined *outside* the parenthesised block below.
rem cmd.exe expands %VAR% when the whole block is parsed, so a `set` inside the
rem block is invisible to later lines in the same block (DIST became empty and
rem windeployqt was called with "\jmcomic_ui.exe").
set "DIST=%HERE%dist"

if /i "%1"=="deploy" (
    if exist "%DIST%" rmdir /s /q "%DIST%"
    mkdir "%DIST%"
    copy /y "%BUILD%\jmcomic_ui.exe" "%DIST%" >nul
    "%QTDIR%\bin\windeployqt.exe" --no-translations --no-system-d3d-compiler ^
        --no-opengl-sw "%DIST%\jmcomic_ui.exe" >nul
    if errorlevel 1 exit /b 1

    rem -----------------------------------------------------------------------
    rem CMake only copies the OpenSSL 4.x DLLs next to the build-tree exe
    rem (POST_BUILD). windeployqt knows nothing about them, and it also brings
    rem its own OpenSSL 3.x for Qt's TLS plugin -- both sets must coexist.
    rem -----------------------------------------------------------------------
    if /i not "%JM_WITH_QUIC%"=="OFF" (
        if exist "%JM_OPENSSL41_ROOT%\bin\libssl-4-x64.dll" (
            copy /y "%JM_OPENSSL41_ROOT%\bin\libssl-4-x64.dll" "%DIST%" >nul
            copy /y "%JM_OPENSSL41_ROOT%\bin\libcrypto-4-x64.dll" "%DIST%" >nul
            if errorlevel 1 (
                echo [x] failed to copy OpenSSL 4.x runtime from %JM_OPENSSL41_ROOT%\bin
                exit /b 1
            )
            echo [ok] OpenSSL 4.x runtime copied from %JM_OPENSSL41_ROOT%\bin
        ) else (
            echo [x] OpenSSL 4.x runtime not found: %JM_OPENSSL41_ROOT%\bin\libssl-4-x64.dll
            echo     Build with "set JM_WITH_QUIC=OFF" or fix JM_OPENSSL41_ROOT.
            exit /b 1
        )
        rem The link must be implicit (import lib), otherwise the DLLs above are
        rem useless and the cold start would silently fall back to no QUIC.
        "%MINGW%\bin\objdump.exe" -p "%DIST%\jmcomic_ui.exe" | findstr /i "libssl-4 libcrypto-4" >nul
        if errorlevel 1 (
            echo [!] exe does not import libssl-4/libcrypto-4 -- check JM_WITH_QUIC
        ) else (
            echo [ok] implicit OpenSSL 4.x imports confirmed
        )
    ) else (
        echo [i] JM_WITH_QUIC=OFF: skipping OpenSSL 4.x runtime copy
    )
    echo [ok] %DIST%\jmcomic_ui.exe
)

endlocal
