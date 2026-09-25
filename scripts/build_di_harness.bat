@echo off
REM build_di_harness.bat - builds build\di_harness.exe, which drives the dinput8 proxy
REM without the game (the same idea as build_harness.bat for the d3d9 proxy).
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "SRC=%ROOT%\src"
set "BUILD=%ROOT%\build"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not exist "%VCVARS%" ( echo [build-dih] ERROR: no vcvars32 & exit /b 1 )
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%" || exit /b 1
call "%VCVARS%" >nul || ( echo [build-dih] ERROR: vcvars32 failed & exit /b 1 )

set "DEFS=/D_CRT_SECURE_NO_WARNINGS /D WIN32_LEAN_AND_MEAN /D NOMINMAX /DDIRECTINPUT_VERSION=0x0800"
set "CFLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 %DEFS%"
echo [build-dih] compiling...
cl %CFLAGS% "%SRC%\di_harness.cpp" || goto :fail
echo [build-dih] linking...
link /nologo /OUT:di_harness.exe di_harness.obj user32.lib dxguid.lib || goto :fail
if not exist di_harness.exe goto :fail
echo [build-dih] OK: %BUILD%\di_harness.exe
exit /b 0
:fail
echo [build-dih] FAILED
exit /b 1
