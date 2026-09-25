@echo off
REM restore.bat - list or restore source snapshots made by snapshot.bat.
REM
REM   scripts\restore.bat                 list the snapshots, newest last
REM   scripts\restore.bat <stamp>         restore that snapshot over src\ and scripts\
REM
REM Restoring overwrites files without asking. It never deletes: a file that exists now but
REM not in the snapshot is left alone, so a restore cannot silently remove work.
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "SNAPS=%ROOT%\_snapshots"

if not exist "%SNAPS%" (
  echo [restore] no snapshots yet - run scripts\snapshot.bat first
  exit /b 1
)

if "%~1"=="" (
  echo [restore] snapshots in %SNAPS%:
  for /f "delims=" %%D in ('dir /b /ad /o-n "%SNAPS%" 2^>nul') do (
    set "N=0"
    for /f %%C in ('dir /b "%%D\src" 2^>nul ^| find /c /v ""') do set "N=%%C"
    echo    %%D    ^(!N! source file(s)^)
  )
  echo.
  echo [restore] restore one with: scripts\restore.bat ^<stamp^>
  exit /b 0
)

set "SRC=%SNAPS%\%~1"
if not exist "%SRC%\src" (
  echo [restore] no such snapshot: %SRC%
  exit /b 1
)

echo [restore] %SRC% -> %ROOT%
robocopy "%SRC%\src" "%ROOT%\src" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail
robocopy "%SRC%\scripts" "%ROOT%\scripts" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail
if exist "%SRC%\README.md" copy /y "%SRC%\README.md" "%ROOT%\README.md" >nul

echo [restore] done - rebuild with scripts\build.bat
exit /b 0

:fail
echo [restore] FAILED
exit /b 1
