@echo off
REM build_copyprobe.bat - builds copyprobe.exe, the headset-free D3D9 copy probe.
REM   It answers one question: which of the calls the compositor's per-frame copy
REM   makes does this machine actually accept? Output: build\_copytest\copyprobe.exe
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "SRC=%ROOT%\src"
set "OUT=%ROOT%\build\_copytest"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not exist "%VCVARS%" (
  echo [copyprobe] ERROR: vcvars32.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1

echo [copyprobe] compiling
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRC%" "%SRC%\copy_probe_main.cpp" /Fe:copyprobe.exe /link user32.lib || goto :fail

echo [copyprobe] OK: %OUT%\copyprobe.exe
exit /b 0
:fail
echo [copyprobe] FAILED
exit /b 1
