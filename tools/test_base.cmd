@echo off
rem Builds and runs tests\test_base.cpp (base.h Run/RunCapture/WriteAtomic) with MSVC v143.
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\base
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /bigobj /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /I"%ROOT%\src" /FI"%ROOT%\tools\msvc_shim.h" "%ROOT%\tests\test_base.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\test_base.exe" /link user32.lib shell32.lib ole32.lib advapi32.lib wtsapi32.lib > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\test_base.exe" || exit /b 1
rem Privacy gate: no location-gated interface in the sources or in the built programs.
python "%ROOT%\tools\check_privacy.py" || exit /b 92
exit /b 0
