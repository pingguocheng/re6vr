@echo off
del "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe0.dll" 1 >nul 2>&1
echo === probe0 exit=%ERRORLEVEL% ===
type "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
del "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe1.dll" 1 >nul 2>&1
echo === probe1 exit=%ERRORLEVEL% ===
type "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
del "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe2.dll" 1 >nul 2>&1
echo === probe2 exit=%ERRORLEVEL% ===
type "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
del "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe3.dll" 1 >nul 2>&1
echo === probe3 exit=%ERRORLEVEL% ===
type "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
del "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
C:\re6vr\build\harness.exe "C:\re6vr\build\_probe\probe4.dll" 1 >nul 2>&1
echo === probe4 exit=%ERRORLEVEL% ===
type "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\probe_trace.txt" 2>nul
