@echo off
setlocal enabledelayedexpansion
set "ROOT=C:\re6vr"
set "TP=%ROOT%\_third_party"
set "OUT=%ROOT%\build\_probe"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%OUT%"
set "BASE=/nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS"
set "LIBS=user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib"
for %%L in (5 6) do (
  cl %BASE% /DPROBE_LEVEL=%%L /I"%TP%\minhook" /c "%ROOT%\scripts\_probe\probe.cpp" /Fo:probe%%L.obj || exit /b 1
  link /nologo /DLL /OUT:probe%%L.dll probe%%L.obj "%TP%\minhook\libminhook.x86.lib" %LIBS% || exit /b 1
  echo built probe%%L.dll
)
