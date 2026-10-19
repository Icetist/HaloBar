@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build\obj mkdir build\obj
rc /nologo /fo build\obj\app.res res\app.rc || exit /b 1
cl /nologo /std:c++20 /O2 /GL /Gw /guard:cf /EHsc /MT /W4 /permissive- /utf-8 /MP ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fo:build\obj\ src\*.cpp build\obj\app.res /Fe:build\HaloBar.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /guard:cf /CETCOMPAT /DYNAMICBASE /NXCOMPAT /MANIFEST:NO ^
   user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib dwmapi.lib ^
   d2d1.lib d3d11.lib dxgi.lib dxguid.lib dwrite.lib dcomp.lib windowscodecs.lib version.lib windowsapp.lib bthprops.lib powrprof.lib wbemuuid.lib uuid.lib || exit /b 1
echo Built %CD%\build\HaloBar.exe
