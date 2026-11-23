@echo off
rem Builds and runs tests\test_image_orient.cpp (FT-1 / B01: DecodeImage, DrawImage and D2DSurface::DibBitmap keep a picture upright)
rem with MSVC v143. Isolated: pictures are generated in memory, a Direct2D offscreen target is used; no window, desktop or setting is touched.
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\image
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%ROOT%\src" /FI"%ROOT%\tools\msvc_shim.h" "%ROOT%\tests\test_image_orient.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\test_image_orient.exe" /link user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib windowscodecs.lib msimg32.lib wtsapi32.lib dwmapi.lib uxtheme.lib comctl32.lib shlwapi.lib version.lib > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\test_image_orient.exe" || exit /b 1
exit /b 0
