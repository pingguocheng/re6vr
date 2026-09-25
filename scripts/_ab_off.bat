@echo off
set "G=C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
if exist "%G%\d3d9.dll" (
  move /y "%G%\d3d9.dll" "%G%\d3d9.dll.off" >nul && echo [ab] proxy disabled
) else (
  echo [ab] no proxy present
)
