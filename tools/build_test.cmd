@echo off
rem AnkhCore isolated TEST build (MSVC v143 x64). Compiles src\ with /DANKH_TEST_BUILD:
rem   - runs unelevated, own data folder %%LOCALAPPDATA%%\AnkhCore-Test, own mutex/window classes
rem   - every system change is refused (see base.h "isolated test build")
rem Output: build-test\out\AnkhCore_TEST.exe  (never copied into Program Files)
setlocal
set ROOT=%~dp0..
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
if exist "%ROOT%\build-test" rmdir /s /q "%ROOT%\build-test"
mkdir "%ROOT%\build-test\res" "%ROOT%\build-test\out"
xcopy /e /q /y "%ROOT%\src\*" "%ROOT%\build-test\" >nul
copy /y "%ROOT%\recovered_res\*" "%ROOT%\build-test\res\" >nul
copy /y "%ROOT%\tools\msvc_shim.h" "%ROOT%\build-test\" >nul
cd /d "%ROOT%\build-test"
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib comctl32.lib dwmapi.lib uxtheme.lib msimg32.lib windowscodecs.lib pdh.lib version.lib shlwapi.lib wbemuuid.lib wtsapi32.lib srclient.lib ntdll.lib
rc /nologo /fo out\app.res app.rc || exit /b 92
cl /nologo /std:c++20 /EHsc /bigobj /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DANKH_TEST_BUILD /FImsvc_shim.h app.cpp out\app.res /Fo:out\ /Fe:out\AnkhCore_TEST.exe /link /SUBSYSTEM:WINDOWS %LIBS% > out\build_app.log 2>&1
set RC=%ERRORLEVEL%
type out\build_app.log | findstr /i /c:"error" /c:"warning"
echo TEST_APP_EXIT=%RC%
exit /b %RC%
