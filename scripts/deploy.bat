@echo off
REM deploy.bat - copies the built proxies into the RE6 install directory.
REM
REM   d3d9.dll          our proxy (the game has no d3d9.dll of its own) - the VR compositor
REM   openxr_loader.dll the OpenXR loader the proxy loads from its own folder
REM   dinput8.dll       the DirectInput8 proxy (head-steered camera) - OPT IN, see below
REM
REM Usage: deploy.bat        install d3d9 + loader, and REMOVE the dinput8 proxy
REM        deploy.bat di     also install the dinput8 proxy
REM
REM The dinput8 proxy is opt-in because it stopped the game from launching once (2026-09-23).
REM It survives a standalone harness (loads, DirectInput8Create returns, CreateDevice goes
REM through the hook and the device vtable is patched), so the failure is specific to running
REM inside RE6 and is not yet understood. Deliberately written with forward jumps only: the
REM first version used nested if/else blocks and cmd.exe rejected them ("else was unexpected")
REM in a way that silently skipped the removal step.
setlocal

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build"
set "TP=%ROOT%\_third_party"
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
set "WITH_DI=0"
if /i "%~1"=="di" set "WITH_DI=1"

if not exist "%GAME%\BH6.exe" goto :nogame
if not exist "%BUILD%\d3d9.dll" goto :nodll

echo [deploy] target: %GAME%
copy /y "%BUILD%\d3d9.dll" "%GAME%\d3d9.dll" || goto :fail
copy /y "%TP%\openxr\loader\x86\openxr_loader.dll" "%GAME%\openxr_loader.dll" || goto :fail

if "%WITH_DI%"=="1" goto :install_di

REM --- default: make sure the dinput8 proxy is NOT in the game folder ---
if exist "%GAME%\dinput8.dll" del /q "%GAME%\dinput8.dll"
if exist "%GAME%\dinput8_orig.dll" del /q "%GAME%\dinput8_orig.dll"
if exist "%GAME%\dinput8.dll.re6vr-disabled" del /q "%GAME%\dinput8.dll.re6vr-disabled"
echo [deploy] dinput8 proxy not installed (run "deploy.bat di" to install it)
goto :report

:install_di
if not exist "%BUILD%\dinput8.dll" goto :nodi
if not exist "%BUILD%\dinput8_orig.dll" goto :nodi
REM BOTH files, always. The proxy's export is a linker FORWARD to dinput8_orig.DirectInput8Create,
REM so the renamed real DLL has to sit beside it - the forwarder is resolved by the loader at
REM load time, and without the target the import fails and the game will not start.
copy /y "%BUILD%\dinput8.dll" "%GAME%\dinput8.dll" || goto :fail
copy /y "%BUILD%\dinput8_orig.dll" "%GAME%\dinput8_orig.dll" || goto :fail
echo [deploy] dinput8 proxy INSTALLED (with dinput8_orig.dll) - head-steered camera available
echo [deploy] WARNING: the earlier version of this proxy stopped the game from starting (stack
echo [deploy]          overflow). This one forwards through a renamed DLL instead. If the game
echo [deploy]          will not start, run "scripts\deploy.bat" to take it back out.

:report
echo [deploy] installed:
if exist "%GAME%\d3d9.dll" for %%A in ("%GAME%\d3d9.dll") do echo    d3d9.dll  %%~zA bytes
if exist "%GAME%\openxr_loader.dll" for %%A in ("%GAME%\openxr_loader.dll") do echo    openxr_loader.dll  %%~zA bytes
if exist "%GAME%\dinput8.dll" for %%A in ("%GAME%\dinput8.dll") do echo    dinput8.dll  %%~zA bytes
echo [deploy] launch the game normally through Steam, then read %GAME%\re6vr.log
exit /b 0

:nogame
echo [deploy] ERROR: RE6 not found at "%GAME%"
exit /b 1

:nodll
echo [deploy] ERROR: %BUILD%\d3d9.dll missing - run build.bat first
exit /b 1

:nodi
echo [deploy] ERROR: build\dinput8.dll missing - run build_dinput8.bat first
exit /b 1

:fail
echo [deploy] FAILED
exit /b 1
