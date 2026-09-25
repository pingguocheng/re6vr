@echo off
REM compare.bat - renders the same synthetic frame through the real per-eye slice
REM geometry for a set of screen settings, so the variants can be compared
REM off-line. Each run leaves re6vr_real_eye*.bgra (998x2148 BGRA) in build\,
REM which scripts\bgra_view.py turns into PNGs.
setlocal
set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build"
cd /d "%BUILD%" || exit /b 1

set RE6VR_SELFTEST=1
set RE6VR_SELFTEST_REAL=1

echo === baseline: 2.5 m / 2.9 m, frustum from the panel (today's default) ===
set RE6VR_SCREEN_DIST=2.5
set RE6VR_SCREEN_WIDTH=2.9
set RE6VR_FRUSTUM_ASPECT_MODE=0
del /q re6vr_real_eye*.bgra 2>nul
harness.exe d3d9.dll 4 1280 720 >nul 2>&1
if exist re6vr_real_eye0.bgra copy /y re6vr_real_eye0.bgra real_base_eye0.bgra >nul
if exist re6vr_real_eye1.bgra copy /y re6vr_real_eye1.bgra real_base_eye1.bgra >nul
python "%ROOT%\scripts\eye_cover.py" re6vr_real_eye0.bgra 998 2148

echo.
echo === cinema: 4 m / 4.6 m, frustum from the panel ===
set RE6VR_SCREEN_DIST=4.0
set RE6VR_SCREEN_WIDTH=4.6
set RE6VR_FRUSTUM_ASPECT_MODE=0
del /q re6vr_real_eye*.bgra 2>nul
harness.exe d3d9.dll 4 1280 720 >nul 2>&1
if exist re6vr_real_eye0.bgra copy /y re6vr_real_eye0.bgra real_cine_eye0.bgra >nul
if exist re6vr_real_eye1.bgra copy /y re6vr_real_eye1.bgra real_cine_eye1.bgra >nul
python "%ROOT%\scripts\eye_cover.py" re6vr_real_eye0.bgra 998 2148

echo.
echo === cinema + viewport-aspect frustum (mode 1) ===
set RE6VR_FRUSTUM_ASPECT_MODE=1
del /q re6vr_real_eye*.bgra 2>nul
harness.exe d3d9.dll 4 1280 720 >nul 2>&1
if exist re6vr_real_eye0.bgra copy /y re6vr_real_eye0.bgra real_mode1_eye0.bgra >nul
python "%ROOT%\scripts\eye_cover.py" re6vr_real_eye0.bgra 998 2148

echo.
echo done - variants: real_base_*, real_cine_*, real_mode1_*
