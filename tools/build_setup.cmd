@echo off
rem AnkhCore 2.5 core installer (MSVC v143 x64). Does not modify src\.
rem   tools\build_setup.cmd [release-inputs folder]    (default: ..\..\release-inputs\from-2.2\SENS)
rem 1. release asset gate on the .rc resources   2. production AnkhCore.exe (tools\build_msvc.cmd)
rem 3. payload from installer\payload.json (asset statuses + SHA-256 enforced)   4. setup builds:
rem    build-setup\out\AnkhCore_2.5_Setup.exe   production (requires administrator)
rem    build-setup\out\AnkhSetup_TEST.exe       test build: unelevated, installs only into its sandbox (--sandbox-test)
setlocal
set ROOT=%~dp0..
set INPUTS=%~1
if "%INPUTS%"=="" set INPUTS=%ROOT%\..\release-inputs\from-2.2\SENS
python -I "%ROOT%\tools\check_release_assets.py" || exit /b 93
call "%ROOT%\tools\build_msvc.cmd" >nul
if errorlevel 1 (echo APP_BUILD_FAILED & exit /b 94)
set VSW="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set PATH=%VSW:"=%;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
if exist "%ROOT%\build-setup" rmdir /s /q "%ROOT%\build-setup"
mkdir "%ROOT%\build-setup\res" "%ROOT%\build-setup\out"
python -I "%ROOT%\tools\pack_payload.py" --app "%ROOT%\build\out\AnkhCore_test.exe" --inputs "%INPUTS%" --out "%ROOT%\build-setup\payload.bin" || exit /b 95
xcopy /e /q /y "%ROOT%\src\*" "%ROOT%\build-setup\" >nul
copy /y "%ROOT%\recovered_res\*" "%ROOT%\build-setup\res\" >nul
copy /y "%ROOT%\tools\msvc_shim.h" "%ROOT%\build-setup\" >nul
cd /d "%ROOT%\build-setup"
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib comctl32.lib dwmapi.lib uxtheme.lib msimg32.lib windowscodecs.lib pdh.lib version.lib shlwapi.lib wbemuuid.lib wtsapi32.lib srclient.lib ntdll.lib bcrypt.lib
set CL_OPTS=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX %ANKH_EXTRA_CL% /FImsvc_shim.h
rc /nologo /fo out\setup.res setup.rc || exit /b 92
cl %CL_OPTS% setup.cpp out\setup.res /Fo:out\ /Fe:out\AnkhCore_2.5_Setup.exe /link /SUBSYSTEM:WINDOWS %LIBS% > out\build_setup.log 2>&1
set RC1=%ERRORLEVEL%
rem test build: same code with ANKH_TEST_BUILD and an asInvoker manifest
powershell -NoProfile -Command "(Get-Content setup.manifest -Raw).Replace('requireAdministrator','asInvoker') | Set-Content -Encoding UTF8 setup.manifest"
rc /nologo /fo out\setup_test.res setup.rc || exit /b 92
cl %CL_OPTS% /DANKH_TEST_BUILD setup.cpp out\setup_test.res /Fo:out\t_ /Fe:out\AnkhSetup_TEST.exe /link /SUBSYSTEM:WINDOWS %LIBS% > out\build_setup_test.log 2>&1
set RC2=%ERRORLEVEL%
findstr /i /c:"error" /c:"warning" out\build_setup.log out\build_setup_test.log
echo SETUP_EXIT=%RC1% SETUP_TEST_EXIT=%RC2%
if not "%RC1%%RC2%"=="00" exit /b 1
exit /b 0
