@echo off
REM run_harness.bat - run the offline proxy harness with a chosen set of environment switches and
REM capture its output to a file, so a crash can be told apart from "it ran and said nothing".
REM
REM Why a .bat: the agent shell is PowerShell, and here-strings / Set-Content are forbidden in this
REM project (they re-encode UTF-8), while `cmd /c "set X=1 && prog"` quoting inside a PowerShell
REM string is a maze. This file is plain ASCII, written once, and used many times.
REM
REM Usage:  scripts\run_harness.bat <out-file> [seconds] [extra harness args...]
setlocal
set "ROOT=%~dp0.."
set "OUT=%~1"
set "SECS=%~2"
if "%OUT%"=="" set "OUT=%ROOT%\_work\harness_log\harness_stdout.txt"
if "%SECS%"=="" set "SECS=5"
shift
shift
set "EXTRA="
:loop
if "%~1"=="" goto :run
set "EXTRA=%EXTRA% %1"
shift
goto :loop
:run
set RE6VR_CAM_SELFTEST=1
set RE6VR_SHOT_SELFTEST=1
set RE6VR_SELFTEST=
"%ROOT%\build\harness.exe" "%ROOT%\build\d3d9.dll" %SECS% 320 240%EXTRA% > "%OUT%" 2>&1
set "RC=%ERRORLEVEL%"
echo [harness] exit code %RC% (output in %OUT%)
exit /b %RC%
