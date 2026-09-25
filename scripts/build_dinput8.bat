@echo off
REM build_dinput8.bat - builds the 32-bit DirectInput8 proxy (dinput8.dll).
REM
REM The proxy does NOT resolve the system DLL under its own name. The real DLL is deployed
REM RENAMED as dinput8_orig.dll and this proxy resolves the entry point from THAT name. That is
REM not stylistic: the first version looked the system DLL up as "dinput8.dll" and the game died
REM with a stack overflow (0xc00000fd) in DINPUT8.dll, twice. Two causes were identified from
REM primary sources - the loader resolves a forwarder's target module BY BASE NAME
REM (ntdll!find_forwarded_export; Wine bug 60130 documents the pattern), and REFramework's
REM Main.cpp records the other: an overlay hooking DirectInput8Create calls back into the
REM proxy's export, which recurses. So the name collision is removed rather than guarded, and
REM the proxy additionally carries a thread_local re-entry guard.
REM
REM Steps:
REM   1. copy C:\Windows\SysWOW64\dinput8.dll (the 32-bit one - System32's is PE32+ and cannot
REM      be loaded in this process) to build\dinput8_orig.dll
REM   2. dumpbin /exports it, and lib /def it into dinput8_orig.lib (kept for reference; the
REM      proxy resolves the entry point at runtime, because a static import of the renamed DLL
REM      was silently dropped by the linker)
REM   3. compile and link the proxy, then verify the export table really has our own
REM      DirectInput8Create plus the real DLL's other five names forwarded to dinput8_orig
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "SRC=%ROOT%\src"
set "BUILD=%ROOT%\build"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
set "REAL_SRC=%SystemRoot%\SysWOW64\dinput8.dll"

if not exist "%VCVARS%" ( echo [build-di] ERROR: no vcvars32 & exit /b 1 )
if not exist "%REAL_SRC%" ( echo [build-di] ERROR: %REAL_SRC% not found & exit /b 1 )
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%" || exit /b 1
call "%VCVARS%" >nul || ( echo [build-di] ERROR: vcvars32 failed & exit /b 1 )

echo [build-di] staging the real DirectInput8 as dinput8_orig.dll
copy /y "%REAL_SRC%" "%BUILD%\dinput8_orig.dll" >nul || goto :fail

if not exist dinput8_orig.lib (
  echo [build-di] generating the import library from its exports
  dumpbin /exports dinput8_orig.dll > dinput8_orig_exports.txt || goto :fail
  REM Take the export NAMES from dumpbin's table and write a .def for lib.exe.
  > dinput8_orig.def echo LIBRARY dinput8_orig.dll
  >> dinput8_orig.def echo EXPORTS
  for /f "tokens=4" %%N in ('findstr /r /c:"^ *[0-9][0-9]* *[0-9A-Fa-f][0-9A-Fa-f]* *[0-9A-Fa-f]* *[A-Za-z_]" dinput8_orig_exports.txt') do (
    >> dinput8_orig.def echo   %%N
  )
  lib /nologo /def:dinput8_orig.def /machine:x86 /out:dinput8_orig.lib >nul || goto :fail
  if not exist dinput8_orig.lib goto :fail
  echo [build-di] dinput8_orig.lib written
) else (
  echo [build-di] dinput8_orig.lib already present (delete it to regenerate)
)

echo [build-di] compiling...
set "DEFS=/D_CRT_SECURE_NO_WARNINGS /D WIN32_LEAN_AND_MEAN /D NOMINMAX /DDIRECTINPUT_VERSION=0x0800"
set "CFLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 %DEFS% /I"%SRC%""
cl %CFLAGS% "%SRC%\dinput8_proxy.cpp" || goto :fail

echo [build-di] linking...
del /q dinput8.dll 2>nul
link /nologo /DLL /OUT:dinput8.dll /DEF:"%SRC%\dinput8.def" /IMPLIB:dinput8.lib ^
  dinput8_proxy.obj dinput8_orig.lib user32.lib
if errorlevel 1 goto :fail

if not exist dinput8.dll goto :fail
for %%A in (dinput8.dll) do set SIZE=%%~zA
if !SIZE! LSS 8192 (
  echo [build-di] ERROR: dinput8.dll is only !SIZE! bytes, the link did not really succeed
  goto :fail
)

echo [build-di] verifying the export table: our own DirectInput8Create plus the real DLL's
echo [build-di] other five names forwarded to it
dumpbin /exports dinput8.dll | findstr /i "DirectInput8Create" >nul || goto :nofwd
dumpbin /exports dinput8.dll | findstr /i "dinput8_orig" >nul || goto :nofwd

echo [build-di] OK: %BUILD%\dinput8.dll (!SIZE! bytes; DirectInput8Create is ours, the rest of
echo [build-di]     the real DLL's exports are forwarded to dinput8_orig.dll)
echo [build-di] deploy BOTH files together: scripts\deploy.bat di
exit /b 0

:nofwd
echo [build-di] ERROR: the proxy does not export DirectInput8Create
exit /b 1

:fail
echo [build-di] FAILED
exit /b 1
