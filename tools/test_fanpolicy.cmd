@echo off
rem Builds AnkhSensors.cs (compile check, same csc.exe the installer uses) and runs the FanPolicy unit tests.
rem References LibreHardwareMonitorLib.dll read-only; nothing touches hardware. Which copy, in this order:
rem   1. %LHM% if set (e.g. an exported release build);
rem   2. the release input the installer ships: ..\release-inputs\from-2.2\SENS (hash-pinned in installer\payload.json);
rem   3. an installed AnkhCore's copy.
rem It no longer needs an installed AnkhCore (2026-11-15: the suite failed after the owner uninstalled Preview 1).
setlocal
set ROOT=%~dp0..
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not defined LHM if exist "%ROOT%\..\release-inputs\from-2.2\SENS\LibreHardwareMonitorLib.dll" set LHM=%ROOT%\..\release-inputs\from-2.2\SENS\LibreHardwareMonitorLib.dll
if not defined LHM set LHM=C:\Program Files\AnkhCore\Sensors\LibreHardwareMonitorLib.dll
if not exist "%LHM%" (echo LibreHardwareMonitorLib.dll not found: %LHM% & exit /b 9)
echo LibreHardwareMonitorLib: %LHM%
set OUT=%ROOT%\build-test\fanpolicy
if not exist "%OUT%" mkdir "%OUT%"
"%CSC%" /nologo /target:winexe /platform:x64 /optimize+ /out:"%OUT%\AnkhSensors_check.exe" /reference:"%LHM%" /reference:System.Management.dll "%ROOT%\src\sensors\AnkhSensors.cs" || exit /b 10
"%CSC%" /nologo /target:exe /platform:x64 /out:"%OUT%\FanPolicyTests.exe" /main:FanPolicyTests /reference:"%LHM%" /reference:System.Management.dll "%ROOT%\src\sensors\AnkhSensors.cs" "%ROOT%\tests\FanPolicyTests.cs" || exit /b 11
copy /y "%LHM%" "%OUT%\" >nul
"%OUT%\FanPolicyTests.exe"
exit /b %ERRORLEVEL%
