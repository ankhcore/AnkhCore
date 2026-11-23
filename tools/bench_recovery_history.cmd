@echo off
rem FT-RH measurement: Recovery History cost before and after (tests\bench_recovery_history.cpp). Built as a TEST build, so
rem it only touches %LOCALAPPDATA%\AnkhCore-Test (its own journal and Recovery folder); it saves and restores the test journal.
rem For the BEFORE numbers set RH_BEFORE_SRC to a copy of src\ taken before the fix.
rem Usage: tools\bench_recovery_history.cmd [before|after] [operations] [entries per operation]
setlocal
set ROOT=%~dp0..
set MODE=%~1
if "%MODE%"=="" set MODE=before
set SRC=%ROOT%\src
set DEFS=
if /i "%MODE%"=="after" set DEFS=/DRH_AFTER
if /i "%MODE%"=="before" if defined RH_BEFORE_SRC set SRC=%RH_BEFORE_SRC%
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 90
set OUT=%ROOT%\build-test\bench-rh-%MODE%
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DANKH_TEST_BUILD %DEFS% /I"%SRC%" /FI"%ROOT%\tools\msvc_shim.h" "%ROOT%\tests\bench_recovery_history.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\bench_rh.exe" /link user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib windowscodecs.lib msimg32.lib wtsapi32.lib dwmapi.lib uxtheme.lib comctl32.lib shlwapi.lib version.lib psapi.lib > "%OUT%\build.log" 2>&1 || (type "%OUT%\build.log" & exit /b 91)
"%OUT%\bench_rh.exe" %2 %3
exit /b %ERRORLEVEL%
