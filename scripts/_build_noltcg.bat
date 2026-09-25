@echo off
setlocal
set "ROOT=C:\re6vr"
set "SRC=%ROOT%\src"
set "TP=%ROOT%\_third_party"
set "BUILD=%ROOT%\build"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%BUILD%"
set "CFLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%TP%\openxr\include" /I"%TP%\minhook""
cl %CFLAGS% "%SRC%\d3d9_proxy.cpp" || exit /b 1
cl %CFLAGS% "%SRC%\openxr_bridge.cpp" || exit /b 1
cl %CFLAGS% "%SRC%\log.cpp" || exit /b 1
cl %CFLAGS% "%SRC%\d3d9_iids.cpp" || exit /b 1
del /q d3d9_noltcg.dll 2>nul
link /nologo /DLL /INCREMENTAL:NO /LTCG:OFF /OUT:d3d9_noltcg.dll /DEF:"%SRC%\d3d9.def" /IMPLIB:d3d9_noltcg.lib d3d9_proxy.obj openxr_bridge.obj log.obj d3d9_iids.obj user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib "%TP%\minhook\libminhook.x86.lib" || exit /b 1
echo built d3d9_noltcg.dll
