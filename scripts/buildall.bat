@echo off
rem Rebuild EVERYTHING in one go: the Debug build (+tests) and the Release +
rem portable folder. The auto-incrementing build number is bumped exactly ONCE
rem for the whole run (the child scripts skip their own bump via GMM_NO_BUMP), so
rem the debug and release binaries share the same version.
rem Run from anywhere -- ROOT is resolved from this script's own location.
setlocal
set "ROOT=%~dp0.."
cd /d "%ROOT%"

rem --- bump the build number once for this run ---
set "BNFILE=%ROOT%\BUILD_NUMBER"
set /a BN=0
if exist "%BNFILE%" set /p BN=<"%BNFILE%"
set /a BN+=1
> "%BNFILE%" echo %BN%
echo === Building GMM version 0.1.%BN% (build %BN%) ===
echo.

set "GMM_NO_BUMP=1"

echo === [1/2] Debug build + tests (build.bat) ===
rem Don't abort the release on a failing test: the debug run is informational.
call "%~dp0build.bat"
echo.

echo === [2/2] Release + portable folder (release.bat) ===
call "%~dp0release.bat" || exit /b 1

echo.
echo ============================================================
echo ALL DONE: GMM 0.1.%BN%
echo   Debug   : %ROOT%\build\GMM.exe
echo   Release : %ROOT%\release\GMM.exe   (single file - ship this one)
echo ============================================================
endlocal
