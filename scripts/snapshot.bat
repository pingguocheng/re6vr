@echo off
REM snapshot.bat - save the source tree before changing it, since this machine has no git.
REM
REM Why this exists: the head-tracking work has been a long sequence of edit / build /
REM deploy / run-with-headset iterations, and several times a change had to be undone with
REM no record of what it replaced. git is not installed here and this directory is not a
REM checkout, so this is the cheap substitute: copy the files that are actually edited into
REM a timestamped folder under _snapshots.
REM
REM Only src\ and scripts\ are copied, plus README.md. build\ (122 MB), _work\ (logs and eye
REM dumps), _logs\ and _third_party\ are generated or vendored, and including them would make
REM a snapshot too large to take at every step - which defeats the purpose.
REM
REM NOTE: this file is deliberately pure ASCII. A .bat with multi-byte characters in it
REM desynchronises cmd.exe's parser, which then chops up the following lines ("setlocal"
REM arrived as "ocal", "REM" as "M"). The first version of this script had a Chinese date
REM string in a comment and failed exactly that way.
setlocal enabledelayedexpansion
set "ROOT=%~dp0.."
set "DEST="

if not "%~1"=="" set "DEST=%ROOT%\_snapshots\%~1"
if not defined DEST (
  set "STAMP=manual"
  for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"`) do set "STAMP=%%T"
  set "DEST=%ROOT%\_snapshots\!STAMP!"
)

echo [snapshot] source to %DEST%
if not exist "%DEST%" mkdir "%DEST%" || exit /b 1

robocopy "%ROOT%\src" "%DEST%\src" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail
robocopy "%ROOT%\scripts" "%DEST%\scripts" /E /NFL /NDL /NJH /NJS /NP /XD _probe >nul
if errorlevel 8 goto :fail
if exist "%ROOT%\README.md" copy /y "%ROOT%\README.md" "%DEST%\README.md" >nul

set "NSRC=0"
for /f %%C in ('dir /b "%DEST%\src\*.cpp" "%DEST%\src\*.h" 2^>nul ^| find /c /v ""') do set "NSRC=%%C"
echo [snapshot] saved %NSRC% source file(s)
echo [snapshot] list or restore with: scripts\restore.bat
exit /b 0

:fail
echo [snapshot] FAILED
exit /b 1
