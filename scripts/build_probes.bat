@echo off
REM build_probes.bat - builds probe0..probe3 DLLs and a tiny x86 host that loads
REM one of them, so a load-time crash can be bisected without involving the game.
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "TP=%ROOT%\_third_party"
set "OUT=%ROOT%\build\_probe"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

call "%VCVARS%" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1

set "INC=/I"%TP%\minhook""
set "BASE=/nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS"
set "LIBS=user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib"

for %%L in (0 1 2 3 4) do (
  cl %BASE% /DPROBE_LEVEL=%%L %INC% /c "%ROOT%\scripts\_probe\probe.cpp" /Fo:probe%%L.obj || goto :fail
  if %%L GEQ 2 (
    link /nologo /DLL /OUT:probe%%L.dll probe%%L.obj "%TP%\minhook\libminhook.x86.lib" %LIBS% || goto :fail
  ) else (
    link /nologo /DLL /OUT:probe%%L.dll probe%%L.obj %LIBS% || goto :fail
  )
  echo [probe] built probe%%L.dll
)

echo [probe] all variants built in %OUT%
exit /b 0
:fail
echo [probe] FAILED
exit /b 1
