@echo off
REM check_redirect.bat - the one link of the redirect path that can be tested without the game.
REM
REM The claim under test: "whatever is drawn while a texture of OURS is bound lands in that texture and
REM reaches the compositor". The engine drawing into it is the other half, and that was already measured
REM (the engine binds its render target outside the render phase and never inside one, 2686 phases, 0
REM binds inside).
REM
REM Usage:  scripts\check_redirect.bat
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
set RE6VR_REDIRECT_SELFTEST=1
set RE6VR_SHARED_SELFTEST=
set RE6VR_STEREO_SELFTEST=
set RE6VR_NO_XR=
"%ROOT%\build\harness.exe" "%ROOT%\build\d3d9.dll" 5 320 240 > "%LOGDIR%\redirect_stdout.txt" 2>&1

findstr /C:"selftest-redirect:" "%LOG%" >nul
if errorlevel 1 (
  echo [redirect] FAIL: the check did not run - see %LOG%
  exit /b 1
)
type "%LOG%" | findstr /C:"selftest-redirect:"
findstr /C:"selftest-redirect: PASS" "%LOG%" >nul
if errorlevel 1 (
  echo [redirect] FAIL: drawing into our own render target did not reach the compositor path
  exit /b 1
)
echo [redirect] PASS: our own render target takes the drawing and reaches the compositor
> "%ROOT%\build\re6vr_logdir.txt" echo %ROOT%\_work
exit /b 0
