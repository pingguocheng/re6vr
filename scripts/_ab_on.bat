@echo off
set "G=C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
if exist "%G%\d3d9.dll.off" (
  move /y "%G%\d3d9.dll.off" "%G%\d3d9.dll" >nul && echo [ab] proxy restored
) else (
  echo [ab] nothing to restore
)
