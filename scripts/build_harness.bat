@echo off
REM build_harness.bat - builds harness.exe (the standalone proxy tester).
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "SRC=%ROOT%\src"
set "BUILD=%ROOT%\build"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

call "%VCVARS%" >nul || exit /b 1
cd /d "%BUILD%" || exit /b 1

echo [harness] compiling
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRC%" "%SRC%\loader.cpp" /Fe:harness.exe /link user32.lib || goto :fail

echo [harness] OK: %BUILD%\harness.exe
exit /b 0
:fail
echo [harness] FAILED
exit /b 1
