@echo off
setlocal
set "SRC=C:\re6vr\src"
set "TP=C:\re6vr\_third_party"
set "BUILD=C:\re6vr\build\_probe"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%BUILD%"
set "CFLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%TP%\openxr\include" /I"%TP%\minhook""
cl %CFLAGS% /DPROXY_SKIP_ATTACH=1 "%SRC%\d3d9_proxy.cpp" /Fo:proxy_skip.obj || exit /b 1
cl %CFLAGS% "%SRC%\openxr_bridge.cpp" /Fo:ob.obj || exit /b 1
cl %CFLAGS% "%SRC%\log.cpp" /Fo:lg.obj || exit /b 1
cl %CFLAGS% "%SRC%\d3d9_iids.cpp" /Fo:iid.obj || exit /b 1
link /nologo /DLL /OUT:proxy_skip.dll /DEF:"%SRC%\d3d9.def" /IMPLIB:proxy_skip.lib proxy_skip.obj ob.obj lg.obj iid.obj user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib "%TP%\minhook\libminhook.x86.lib" || exit /b 1
echo built proxy_skip.dll
