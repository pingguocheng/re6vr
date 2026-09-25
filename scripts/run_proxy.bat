@echo off
REM run_proxy.bat - run the harness against an ARBITRARY d3d9.dll with the camera self-test armed.
REM
REM Why it exists: the camera self-test (`RE6VR_CAM_SELFTEST=1`) crashes the harness process
REM (exit 0xC0000005) with the build of 2026-09-25 11:0x, while the same harness without that
REM switch runs clean. The crash is therefore in a path the self-test enables, and the first thing
REM to establish is whether it is NEW (introduced by the camera-read-path work) or was always
REM there, which this script answers by running the same test against an older d3d9.dll.
REM
REM Usage:  scripts\run_proxy.bat <d3d9.dll> [out-file]
setlocal
set "ROOT=%~dp0.."
set "PROXY=%~1"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%ROOT%\_work\harness_log\proxyrun.txt"
set RE6VR_CAM_SELFTEST=1
set RE6VR_SHOT_SELFTEST=
set RE6VR_SELFTEST=
echo [run_proxy] proxy=%PROXY%
"%ROOT%\build\harness.exe" "%PROXY%" 1 320 240 > "%OUT%" 2>&1
set "RC=%ERRORLEVEL%"
echo [run_proxy] exit %RC% (output in %OUT%)
exit /b %RC%
