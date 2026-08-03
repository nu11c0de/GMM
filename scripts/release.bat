@echo off
rem Build a redistributable Release: a portable folder (release\gtamm\, for
rem reference/debugging) AND the actual single-file deliverable release\GMM.exe
rem (a tiny launcher that embeds the whole portable folder and self-extracts
rem next to itself on first run -- see launcher\Launcher.cpp).
rem Run from anywhere -- ROOT is resolved from this script's own location.
setlocal
set "ROOT=%~dp0.."
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
set "QTKIT=C:\Qt\6.8.3\msvc2022_64"
set "CMAKEBIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CMAKEBIN%\CMake\bin;%CMAKEBIN%\Ninja;%QTKIT%\bin;%PATH%"
set "CMAKE_PREFIX_PATH=%QTKIT%"
cd /d "%ROOT%"

rem --- bump the auto-incrementing build number (baked into the version header) ---
rem buildall.bat bumps once for the whole run and sets GMM_NO_BUMP to skip this.
if defined GMM_NO_BUMP goto after_bump
set "BNFILE=%ROOT%\BUILD_NUMBER"
set /a BN=0
if exist "%BNFILE%" set /p BN=<"%BNFILE%"
set /a BN+=1
> "%BNFILE%" echo %BN%
echo Build number: %BN%  (version 0.1.%BN%)
:after_bump

rem --- Pass 1: Release build of the real program (separate dir so Debug is untouched) ---
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" || exit /b 1
cmake --build build-release --target gtamm gtamm-gui || exit /b 1

rem --- Assemble the portable folder (payload for the launcher, pass 2) ---
set "OUT=%ROOT%\release\gtamm"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
copy /y "build-release\gtamm.exe" "%OUT%" >nul
copy /y "build-release\GMM.exe"   "%OUT%" >nul
copy /y "%ROOT%\README.md"        "%OUT%" >nul
rem MIT license + third-party notices must ship with the binaries (Qt's LGPL
rem terms and libarchive's BSD notice require it; MIT requires its own too).
copy /y "%ROOT%\LICENSE"                "%OUT%" >nul
copy /y "%ROOT%\THIRD-PARTY-NOTICES.md" "%OUT%" >nul

rem The Mod Loader runtime is embedded INTO GMM.exe (from runtime\) and unpacked
rem next to the program at first launch, so nothing needs to be copied here.

rem Qt runtime + plugins + the MSVC runtime (so no separate VC++ redist is needed)
"%QTKIT%\bin\windeployqt.exe" --release --compiler-runtime --no-translations ^
  "%OUT%\GMM.exe" || exit /b 1

rem Non-Qt dependency DLLs (libarchive + zlib/lzma/bz2/zstd/openssl) from the build
for %%F in (build-release\*.dll) do copy /y "%%F" "%OUT%" >nul

rem Ship the loose MSVC runtime DLLs so the folder runs with no VC++ install;
rem drop the redist installer that windeployqt copied.
del /q "%OUT%\vc_redist.x64.exe" 2>nul
for /d %%D in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do copy /y "%%D\*.dll" "%OUT%" >nul

rem --- Pack the portable folder into launcher\payload.zip -----------------
rem STORE only (-mx=0 / NoCompression): the launcher's own extractor is a tiny
rem hand-rolled reader with zero DLL dependencies (see launcher\Launcher.cpp)
rem -- it deliberately only understands the uncompressed zip method, so it
rem never needs to link libarchive/zlib and stays a true single file. Trades
rem a larger payload for that.
set "PAYLOAD=%ROOT%\launcher\payload.zip"
del /q "%PAYLOAD%" 2>nul
set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
if exist "%SEVENZIP%" (
  pushd "%OUT%"
  "%SEVENZIP%" a -tzip -mx=0 "%PAYLOAD%" * >nul || (popd & exit /b 1)
  popd
) else (
  powershell -NoProfile -Command ^
    "Compress-Archive -Path '%OUT%\*' -DestinationPath '%PAYLOAD%' -CompressionLevel NoCompression -Force" || exit /b 1
)
for %%Z in ("%PAYLOAD%") do echo Payload packed: %%~zZ bytes

rem --- Pass 2: reconfigure (payload.zip now exists -> the launcher target
rem appears) and build ONLY the launcher; everything else is already built. ---
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" || exit /b 1
cmake --build build-release --target gtamm-launcher || exit /b 1

copy /y "build-release\gtamm-launcher.exe" "%ROOT%\release\GMM.exe" >nul

echo.
echo RELEASE READY:
echo   %ROOT%\release\GMM.exe      ^<- SHIP THIS ONE FILE. Self-extracts next to
echo                                   itself into bin\ + data\ on first run.
echo   %ROOT%\release\gtamm\       ^<- portable folder (payload source / debugging
echo                                   only -- not what you hand out).
endlocal
