@echo off
rem FT-2 / A04 measurement: Icon Manager paint cost before and after (tests\bench_icon_manager.cpp). Read-only: it reads the
rem installed theme folders and writes nothing. Usage: tools\bench_icon_manager.cmd [old|new] [theme root]
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\bench
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%ROOT%\src" /FI"%ROOT%\tools\msvc_shim.h" "%ROOT%\tests\bench_icon_manager.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\bench_icon_manager.exe" /link user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib windowscodecs.lib msimg32.lib wtsapi32.lib dwmapi.lib uxtheme.lib comctl32.lib shlwapi.lib version.lib psapi.lib > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\bench_icon_manager.exe" %*
exit /b %ERRORLEVEL%
