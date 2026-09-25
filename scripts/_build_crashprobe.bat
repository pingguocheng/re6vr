@echo off
setlocal
set "SRC=C:\re6vr\src"
set "OUT=C:\re6vr\build\_probe"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%OUT%"
cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS "%SRC%\crash_probe_main.cpp" /Fe:crashprobe.exe /link kernel32.lib || exit /b 1
echo built crashprobe.exe
