@echo off
REM check_shared.bat - can this machine share a D3D9Ex texture with D3D11 at all?
REM
REM The whole no-readback stereo design rests on one interop: a D3D9Ex texture created WITH a shared
REM handle, opened in D3D11 by OpenSharedResource, sampled directly. If that works, the per-frame
REM GetRenderTargetData/LockRect/Map chain - the machinery every crash of 2026-09-25 landed in - can be
REM deleted. If it does not, the design is dead and no amount of per-frame debugging would have said so.
REM
REM It needs no game and no headset: the proxy creates its own D3D9Ex device, fills a shared texture
REM through D3D9, opens it in D3D11 and reads the pixel back in D3D11.
REM
REM Usage:  scripts\check_shared.bat
setlocal
set "ROOT=%~dp0.."
set "LOGDIR=%ROOT%\_work\harness_log"
set "LOG=%LOGDIR%\re6vr.log"

if not exist "%LOGDIR%" mkdir "%LOGDIR%"
> "%LOGDIR%\re6vr_stereo.txt" echo 1
> "%LOGDIR%\re6vr_view.txt" echo off
> "%LOGDIR%\re6vr_rt_probe.txt" echo 0
> "%ROOT%\build\re6vr_logdir.txt" echo %LOGDIR%
del /q "%LOG%" 2>nul

set RE6VR_SELFTEST=1
set RE6VR_SHARED_SELFTEST=1
set RE6VR_STEREO_SELFTEST=
set RE6VR_NO_XR=
"%ROOT%\build\harness.exe" "%ROOT%\build\d3d9.dll" 5 320 240 > "%LOGDIR%\shared_stdout.txt" 2>&1

findstr /C:"selftest-shared:" "%LOG%" >nul
if errorlevel 1 (
  echo [shared] FAIL: the check did not run - see %LOG%
  exit /b 1
)
findstr /C:"selftest-shared: D3D11 read" "%LOG%" >nul
if errorlevel 1 (
  echo [shared] FAIL: no readback verdict - see %LOG%
  type "%LOG%" | findstr /C:"selftest-shared:"
  exit /b 1
)
type "%LOG%" | findstr /C:"selftest-shared:"
findstr /C:"-> PASS" "%LOG%" >nul
if errorlevel 1 (
  echo [shared] FAIL: D3D9Ex <-> D3D11 texture sharing does NOT work on this machine
  exit /b 1
)
echo [shared] PASS: a D3D9Ex shared texture is readable from D3D11
> "%ROOT%\build\re6vr_logdir.txt" echo %ROOT%\_work
exit /b 0
