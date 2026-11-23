@echo off
rem Builds and runs tests\test_candy_bg.cpp (Candy presentation background: matte, blur, cache) with MSVC v143. Pure logic, no window.
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\candybg
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%ROOT%\src" "%ROOT%\tests\test_candy_bg.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\test_candy_bg.exe" > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\test_candy_bg.exe" || exit /b 1
exit /b 0
