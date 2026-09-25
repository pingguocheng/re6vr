@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d C:\re6vr\build
cl /nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"C:\re6vr\src" /I"C:\re6vr\_third_party\openxr\include" /I"C:\re6vr\_third_party\minhook" "C:\re6vr\src\d3d9_proxy.cpp" 2>&1
