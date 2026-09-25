@echo off
REM uninstall.bat - removes the proxy from the RE6 install directory.
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"

if exist "%GAME%\d3d9.dll" (
  del /q "%GAME%\d3d9.dll" && echo [uninstall] removed d3d9.dll
) else (
  echo [uninstall] d3d9.dll not present
)
if exist "%GAME%\openxr_loader.dll" (
  del /q "%GAME%\openxr_loader.dll" && echo [uninstall] removed openxr_loader.dll
) else (
  echo [uninstall] openxr_loader.dll not present
)
if exist "%GAME%\re6vr.log" echo [uninstall] kept %GAME%\re6vr.log for inspection
exit /b 0
