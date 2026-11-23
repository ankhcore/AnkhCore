@echo off
rem AnkhCore 2.2 baseline test build (MSVC, VS 2022 x64). Does not modify src\.
rem Stages src\ + recovered_res\ into build\ and compiles there. Output: build\out\AnkhCore_test.exe
setlocal
set ROOT=%~dp0..
set VSW="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set PATH=%VSW:"=%;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
if exist "%ROOT%\build" rmdir /s /q "%ROOT%\build"
mkdir "%ROOT%\build\res" "%ROOT%\build\out"
xcopy /e /q /y "%ROOT%\src\*" "%ROOT%\build\" >nul
copy /y "%ROOT%\recovered_res\*" "%ROOT%\build\res\" >nul
copy /y "%ROOT%\tools\msvc_shim.h" "%ROOT%\build\" >nul
cd /d "%ROOT%\build"
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib comctl32.lib dwmapi.lib uxtheme.lib msimg32.lib windowscodecs.lib pdh.lib version.lib shlwapi.lib wbemuuid.lib wtsapi32.lib srclient.lib ntdll.lib
rc /nologo /fo out\app.res app.rc || exit /b 92
cl /nologo /std:c++20 /EHsc /bigobj /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX %ANKH_EXTRA_CL% /FImsvc_shim.h app.cpp out\app.res /Fo:out\ /Fe:out\AnkhCore_test.exe /link /SUBSYSTEM:WINDOWS %LIBS% > out\build_app.log 2>&1
set RC=%ERRORLEVEL%
echo APP_EXIT=%RC%
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX %ANKH_EXTRA_CL% /FImsvc_shim.h /c setup.cpp /Fo:out\setup.obj > out\build_setup.log 2>&1
echo SETUP_COMPILE_EXIT=%ERRORLEVEL%
exit /b %RC%
