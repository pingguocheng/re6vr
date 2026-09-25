@echo off
REM check_nostereo.bat - the regression gate for the stereo work: with re6vr_stereo.txt = 0 the proxy
REM must behave exactly as it did before there was a stereo path at all.
REM
REM It asserts two things in the log: the per-eye surfaces were NOT created, and the panel self-test
REM still PASSes (that is the check that caught the self-test/capture sharing shared_srv).
REM
REM Usage:  scripts\check_nostereo.bat
setlocal
set "ROOT=%~dp0.."
set "LOGDIR=%ROOT%\_work\harness_log"
set "LOG=%LOGDIR%\re6vr.log"

if not exist "%LOGDIR%" mkdir "%LOGDIR%"
> "%LOGDIR%\re6vr_stereo.txt" echo 0
> "%LOGDIR%\re6vr_view.txt" echo off
> "%LOGDIR%\re6vr_rt_probe.txt" echo 0
> "%ROOT%\build\re6vr_logdir.txt" echo %LOGDIR%
del /q "%LOG%" 2>nul

set RE6VR_SELFTEST=1
set RE6VR_STEREO_SELFTEST=
set RE6VR_NO_XR=
set RE6VR_CAM_SELFTEST=
"%ROOT%\build\harness.exe" "%ROOT%\build\d3d9.dll" 5 320 240 > "%LOGDIR%\nostereo_stdout.txt" 2>&1

findstr /C:"stereo surfaces NOT created" "%LOG%" >nul
if errorlevel 1 (
  echo [nostereo] FAIL: the log does not say the stereo surfaces were skipped
  exit /b 1
)
findstr /C:"selftest: PASS" "%LOG%" >nul
if errorlevel 1 (
  echo [nostereo] FAIL: the panel self-test did not pass with stereo off
  exit /b 1
)
findstr /C:"STEREO capture armed" "%LOG%" >nul
if not errorlevel 1 (
  echo [nostereo] FAIL: stereo surfaces were created even though re6vr_stereo.txt was 0
  exit /b 1
)
echo [nostereo] PASS: stereo inert (no per-eye surfaces) and the panel self-test still passes

REM Leave the marker directory ready for the next offline run, and point the log back at _work so a
REM game launch does not write into the harness folder.
> "%LOGDIR%\re6vr_stereo.txt" echo 1
> "%ROOT%\build\re6vr_logdir.txt" echo %ROOT%\_work
exit /b 0
