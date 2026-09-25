@echo off
REM run_to2.bat - isolated text-mode runs of the harness to see whether an access violation tells
REM them apart. It was written after `PROCESSOR_ARCHITECTURE=AMD64` turned out to break the mode
REM switch; these runs exist to prove or disprove that the same class of bug is what killed the
REM 10:52 batch runs.
setlocal
set "H=%~dp0..\build\harness.exe"
set "D=%~dp0..\build\d3d9.dll"
echo === A: no switches ===
set RE6VR_CAM_SELFTEST=
set RE6VR_SHOT_SELFTEST=
set RE6VR_SELFTEST=
"%H%" "%D%" 1 320 240 > "%~dp0..\_work\harness_log\A.txt" 2>&1
echo    exit %ERRORLEVEL%
echo === B: cam selftest only ===
set RE6VR_CAM_SELFTEST=1
set RE6VR_SHOT_SELFTEST=
"%H%" "%D%" 1 320 240 > "%~dp0..\_work\harness_log\B.txt" 2>&1
echo    exit %ERRORLEVEL%
echo === C: shot selftest only ===
set RE6VR_CAM_SELFTEST=
set RE6VR_SHOT_SELFTEST=1
"%H%" "%D%" 1 320 240 > "%~dp0..\_work\harness_log\C.txt" 2>&1
echo    exit %ERRORLEVEL%
echo === D: both ===
set RE6VR_CAM_SELFTEST=1
set RE6VR_SHOT_SELFTEST=1
"%H%" "%D%" 1 320 240 > "%~dp0..\_work\harness_log\D.txt" 2>&1
echo    exit %ERRORLEVEL%
exit /b 0
