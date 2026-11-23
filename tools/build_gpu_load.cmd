@echo off
rem Builds tools\gpu_load.cpp (test tool for tools\gpu_verify.ps1) with MSVC v143. Output: build-test\gpu\gpu_load.exe
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\gpu
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE "%ROOT%\tools\gpu_load.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\gpu_load.exe" > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
echo GPU_LOAD_EXIT=0
