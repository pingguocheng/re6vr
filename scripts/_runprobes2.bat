@echo off
cd /d C:\re6vr\build\_probe
del probe_trace.txt probe_logger.txt 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe5.dll" 1 >nul 2>&1
echo === probe5 exit=%ERRORLEVEL% ===
type probe_trace.txt 2>nul
del probe_trace.txt probe_logger.txt 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe6.dll" 1 >nul 2>&1
echo === probe6 exit=%ERRORLEVEL% ===
type probe_trace.txt 2>nul
