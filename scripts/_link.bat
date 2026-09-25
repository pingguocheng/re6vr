@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d C:\re6vr\build
del d3d9.dll 2>nul
link /nologo /DLL /OUT:d3d9.dll /DEF:"C:\re6vr\src\d3d9.def" /IMPLIB:d3d9.lib d3d9_proxy.obj openxr_bridge.obj log.obj d3d9_iids.obj user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib "C:\re6vr\_third_party\minhook\libminhook.x86.lib"
echo LINK_EXIT=%ERRORLEVEL%
dir /b d3d9.dll
