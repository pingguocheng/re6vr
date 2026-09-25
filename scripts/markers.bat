@echo off
REM markers.bat - set the in-game test switches without needing elevation.
REM
REM The game's own folder belongs to Steam and is not writable by a normal user,
REM so every switch used to cost an elevated copy. A one-line re6vr_logdir.txt
REM beside the proxy moves the log and ALL marker files into that directory.
REM This script writes the markers there and prints what it set.
REM
REM Usage:  scripts\markers.bat                      show the current state
REM         scripts\markers.bat auto                 per-eye trim: auto (either sign)
REM         scripts\markers.bat auto-                per-eye trim: auto, other sign
REM         scripts\markers.bat uv 0 0               per-eye trim: explicit percent
REM         scripts\markers.bat ipd 1                stereo separation (0 = shared eye)
REM         scripts\markers.bat ipd 9                converged diagnostic (same pose both eyes)
REM         scripts\markers.bat two 1                one swapchain per eye
REM         scripts\markers.bat two 0                one array swapchain, two slices
REM         scripts\markers.bat dist 5.5             screen distance in metres
REM         scripts\markers.bat width 4.6            screen width in metres
REM         scripts\markers.bat scale 1.5            apparent size on the display (1 = 4 m fills it)
REM         scripts\markers.bat solid half           panel shows red|green + white line
REM         scripts\markers.bat solid off            panel shows the game again
REM         scripts\markers.bat capture off          stop the vertex-shader probe
REM         scripts\markers.bat cam 1                class-pointer camera probe (read-only)
REM         scripts\markers.bat reset                clear every marker
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
REM The proxy reads its marker files from its own directory unless
REM re6vr_logdir.txt (beside the proxy) names another one - and this project's
REM re6vr_logdir.txt points at C:\re6vr\_work. Writing the markers anywhere
REM else silently does nothing, because the game finds no marker and quietly keeps
REM its default. Override with RE6VR_MARKER_DIR when testing a build of your own.
set "WORK=%RE6VR_MARKER_DIR%"
if not "%WORK%"=="" goto :have_work
if exist "%ROOT%\build\re6vr_logdir.txt" set /p WORK=<"%ROOT%\build\re6vr_logdir.txt"
if "%WORK%"=="" set "WORK=%ROOT%\_work"
:have_work
if not exist "%WORK%" mkdir "%WORK%" 2>nul

if "%~1"=="" goto :show

if /i "%~1"=="auto"  ( > "%WORK%\re6vr_uv_shift.txt" echo auto  & echo [markers] uv shift = auto )
if /i "%~1"=="auto-" ( > "%WORK%\re6vr_uv_shift.txt" echo auto- & echo [markers] uv shift = auto- )
if /i "%~1"=="uv"    ( > "%WORK%\re6vr_uv_shift.txt" echo %~2 %~3 & echo [markers] uv shift = %~2 %~3 )
if /i "%~1"=="ipd"   ( > "%WORK%\re6vr_ipd_scale.txt" echo %~2 & echo [markers] ipd_scale = %~2 )
if /i "%~1"=="two"   ( > "%WORK%\re6vr_two_swapchains.txt" echo %~2 & echo [markers] two_swapchains = %~2 )
if /i "%~1"=="solid" ( > "%WORK%\re6vr_test_solid.txt" echo %~2 & echo [markers] test_solid = %~2 )
if /i "%~1"=="capture" ( > "%WORK%\re6vr_capture_vs.txt" echo %~2 & echo [markers] capture_vs = %~2 )
if /i "%~1"=="dist"  ( > "%WORK%\re6vr_screen_dist.txt" echo %~2 & echo [markers] screen_dist = %~2 )
if /i "%~1"=="width" ( > "%WORK%\re6vr_screen_width.txt" echo %~2 & echo [markers] screen_width = %~2 )
if /i "%~1"=="scale" ( > "%WORK%\re6vr_screen_scale.txt" echo %~2 & echo [markers] screen_scale = %~2 )
if /i "%~1"=="follow" ( > "%WORK%\re6vr_screen_follow.txt" echo %~2 & echo [markers] screen_follow = %~2 )
REM head <gain> [<target>]: rotate the game's view matrix by the head orientation.
REM target: auto (find the camera by watching which slot moves) / all / <index>.
if /i "%~1"=="head" (
  if "%~3"=="" ( > "%WORK%\re6vr_head_view.txt" echo %~2 ) else ( > "%WORK%\re6vr_head_view.txt" echo %~2 %~3 )
  echo [markers] head_view = %~2 %~3
)
if /i "%~1"=="head" if /i "%~2"=="off" del /q "%WORK%\re6vr_head_view.txt" 2>nul
REM trace 1: log every view candidate each frame, to find which register is the camera
if /i "%~1"=="trace" ( > "%WORK%\re6vr_trace.txt" echo %~2 & echo [markers] trace = %~2 )
REM --- head-steered camera through the DirectInput8 proxy ---
REM di on / di off: add the head's yaw to the right stick (the game's own camera controller
REM                 then turns the view - no camera object needed).
REM di axis <ofs>: which axis to drive, as a DIJOFS byte offset:
REM                 0=lX 4=lY 8=lZ 12=lRx 16=lRy 20=lRz. Find it from the di: state log.
REM di gain <g>  : degrees of head turn per stick full deflection is set by scale; gain
REM                 multiplies on top.
if /i "%~1"=="di" (
  if /i "%~2"=="axis" ( > "%WORK%\re6vr_di_axis.txt" echo %~3 & echo [markers] di axis = %~3 )
  if /i "%~2"=="gain" ( > "%WORK%\re6vr_di_gain.txt" echo %~3 & echo [markers] di gain = %~3 )
  if /i "%~2"=="scale" ( > "%WORK%\re6vr_di_scale.txt" echo %~3 & echo [markers] di scale = %~3 )
  if /i "%~2"=="on"  ( > "%WORK%\re6vr_di_inject.txt" echo 1 & echo [markers] di inject = 1 )
  if /i "%~2"=="off" ( > "%WORK%\re6vr_di_inject.txt" echo 0 & echo [markers] di inject = 0 )
)
REM scan 1: hunt for the camera OBJECT - captures two camera poses and searches memory
REM         for the address holding both. Walk around during the 3 s between captures.
if /i "%~1"=="scan" ( > "%WORK%\re6vr_scan.txt" echo %~2 & echo [markers] scan = %~2 )
REM cam 1: the CLASS-POINTER route (no walking needed, read-only): waits 20 s for a stage,
REM        then reads [0x017D270C] / [stage+0x640] and walks memory for objects whose class
REM        record is sBioCamera, checking each one's mCameraOrg[0] geometry. One run answers
REM        "is the camera object live, and are the offsets right".
if /i "%~1"=="cam" ( > "%WORK%\re6vr_cam.txt" echo %~2 & echo [markers] cam probe = %~2 )
REM camhook 1: hook the engine's own mCameraOrg writer (0x004FF9B0, found statically) and let it
REM           hand over the camera object address, the pose it writes and its source group.
REM           No memory search, no identity key - the engine names the object itself.
if /i "%~1"=="camhook" ( > "%WORK%\re6vr_camhook.txt" echo %~2 & echo [markers] cam hook = %~2 )

if /i "%~1"=="reset" (
  echo [markers] clearing every marker
  for %%F in (re6vr_uv_shift.txt re6vr_ipd_scale.txt re6vr_two_swapchains.txt
              re6vr_test_solid.txt re6vr_capture_vs.txt re6vr_screen_dist.txt
              re6vr_screen_width.txt re6vr_screen_scale.txt re6vr_screen_follow.txt
              re6vr_head_view.txt re6vr_trace.txt re6vr_scan.txt re6vr_swapchain.txt
              re6vr_di_inject.txt re6vr_di_axis.txt re6vr_di_gain.txt re6vr_di_scale.txt
              re6vr_cam.txt re6vr_camhook.txt re6vr_dump_at.txt) do (
    if exist "%WORK%\%%F" del /q "%WORK%\%%F"
  )
)
goto :show

:show
echo.
echo [markers] directory: %WORK%
echo [markers] proxy reads them only if re6vr_logdir.txt sits next to d3d9.dll
echo [markers] and names this directory.
echo.
for %%F in (re6vr_uv_shift.txt re6vr_ipd_scale.txt re6vr_two_swapchains.txt
            re6vr_test_solid.txt re6vr_capture_vs.txt re6vr_screen_dist.txt
            re6vr_screen_width.txt re6vr_screen_scale.txt re6vr_screen_follow.txt
            re6vr_head_view.txt re6vr_trace.txt re6vr_scan.txt re6vr_swapchain.txt
            re6vr_di_inject.txt re6vr_di_axis.txt re6vr_di_gain.txt re6vr_di_scale.txt
            re6vr_cam.txt re6vr_camhook.txt re6vr_dump_at.txt) do (
  if exist "%WORK%\%%F" (
    set "VAL="
    set /p VAL=<"%WORK%\%%F"
    echo    %%~nF = !VAL!
  ) else (
    echo    %%~nF = (unset^)
  )
)
endlocal
