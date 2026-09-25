@echo off
REM build.bat - builds the 32-bit d3d9 proxy for RE6 (Resident Evil 6).
REM   * compiles every .cpp in src\ with the MSVC x86 toolset
REM   * links them, together with MinHook, into build\d3d9.dll
REM   * refuses to report success unless the DLL was actually produced
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SRC=%ROOT%\src"
set "TP=%ROOT%\_third_party"
set "BUILD=%ROOT%\build"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not exist "%VCVARS%" (
  echo [build] ERROR: vcvars32.bat not found at "%VCVARS%"
  exit /b 1
)
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%" || exit /b 1

echo [build] entering x86 build environment
call "%VCVARS%" >nul
if errorlevel 1 (
  echo [build] ERROR: vcvars32 failed
  exit /b 1
)

set "INCS=/I"%SRC%" /I"%TP%\openxr\include" /I"%TP%\minhook""
set "DEFS=/D_CRT_SECURE_NO_WARNINGS /D WIN32_LEAN_AND_MEAN /D NOMINMAX"
set "CFLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 %DEFS% %INCS%"
set "LIBS=user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib "%TP%\minhook\libminhook.x86.lib""

echo [build] compiling...
for %%F in (d3d9_proxy openxr_bridge matrix_probe view_probe cam_steer mem_scan mem_cam cam_hook screenshot log d3d9_iids) do (
  cl %CFLAGS% "%SRC%\%%F.cpp" || goto :fail
)

echo [build] linking...
del /q d3d9.dll 2>nul
link /nologo /DLL /OUT:d3d9.dll /DEF:"%SRC%\d3d9.def" /IMPLIB:d3d9.lib ^
  d3d9_proxy.obj openxr_bridge.obj matrix_probe.obj view_probe.obj cam_steer.obj mem_scan.obj mem_cam.obj cam_hook.obj screenshot.obj log.obj d3d9_iids.obj %LIBS%
if errorlevel 1 goto :fail

if not exist d3d9.dll goto :fail
for %%A in (d3d9.dll) do set SIZE=%%~zA
if !SIZE! LSS 10000 (
  echo [build] ERROR: d3d9.dll is only !SIZE! bytes, the link did not really succeed
  goto :fail
)

echo [build] OK: %BUILD%\d3d9.dll (!SIZE! bytes)
echo [build] tip: a re6vr_logdir.txt beside the proxy moves the log and every
echo [build]      marker file into the directory it names - see scripts\markers.bat
exit /b 0

:fail
echo [build] FAILED
exit /b 1
