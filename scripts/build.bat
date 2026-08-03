@echo off
rem Build gtamm using the bundled Visual Studio 18 Insiders toolchain.
rem Run from anywhere -- ROOT is resolved from this script's own location.
setlocal
set "ROOT=%~dp0.."
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
set "CMAKEBIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CMAKEBIN%\CMake\bin;%CMAKEBIN%\Ninja;%PATH%"
rem Qt6 kit for the GUI target (installed via aqtinstall).
set "CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64"
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

cmake --preset default || exit /b 1
cmake --build build || exit /b 1
echo.
echo BUILD OK: %ROOT%\build\gtamm.exe
echo.
echo Running tests...
"%ROOT%\build\gtamm_tests.exe" --reporter compact || exit /b 1
endlocal
