@echo off
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\cleaner
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%ROOT%\src" /FI"%ROOT%\tools\msvc_shim.h" "%ROOT%\tests\test_cleaner.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\test_cleaner.exe" /link user32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib wbemuuid.lib wtsapi32.lib srclient.lib shlwapi.lib > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\test_cleaner.exe" || exit /b 1
exit /b 0
