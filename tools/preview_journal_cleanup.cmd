@echo off
rem ============================================================================================================
rem AnkhCore PREVIEW (test build) - clean the accumulated change journal. Stable / installed data is never touched.
rem
rem What it changes: ONE file, %LOCALAPPDATA%\AnkhCore-Test\journal.txt (the Recovery History of the test build,
rem the one that listed about 9,000 changes). It first makes a verified backup copy next to it, then deletes that single
rem file. AnkhCore recreates an empty journal at the next recorded change.
rem What it does NOT touch: the installed AnkhCore data (C:\ProgramData\AnkhCore: journal, Recovery, Backup, Logs),
rem any .acbackup, settings.json, the Recovery\ folder, logs, or any other file. No wildcard, no del /s, no folder removal.
rem
rem Usage:   preview_journal_cleanup.cmd /dry        shows everything it would do, changes nothing
rem          preview_journal_cleanup.cmd             asks for confirmation, then backs up and cleans
rem          preview_journal_cleanup.cmd /restore    puts the newest backup back (asks first)
rem ============================================================================================================
setlocal EnableExtensions DisableDelayedExpansion

set "TARGET_DIR=%LOCALAPPDATA%\AnkhCore-Test"
set "TARGET=%TARGET_DIR%\journal.txt"
set "STABLE_DIR=%ProgramData%\AnkhCore"
set "STABLE=%STABLE_DIR%\journal.txt"
set "MODE=%~1"

echo.
echo   Preview (test build) journal : %TARGET%
echo   Stable / installed journal   : %STABLE%   (never modified by this script)
echo.

rem --- guards ---------------------------------------------------------------------------------------------
if "%LOCALAPPDATA%"=="" (echo ERROR: LOCALAPPDATA is not set. Nothing was changed.& exit /b 2)
if /i "%TARGET_DIR%"=="%STABLE_DIR%" (echo ERROR: the Preview and Stable folders are the same. Nothing was changed.& exit /b 2)
if /i "%TARGET%"=="%STABLE%" (echo ERROR: the target is the Stable journal. Nothing was changed.& exit /b 2)
if /i not "%TARGET%"=="%LOCALAPPDATA%\AnkhCore-Test\journal.txt" (echo ERROR: unexpected target. Nothing was changed.& exit /b 2)

if /i "%MODE%"=="/restore" goto :restore

if not exist "%TARGET%" (echo Nothing to clean: the Preview journal does not exist.& exit /b 0)

tasklist /fi "imagename eq AnkhCore_TEST.exe" 2>nul | find /i "AnkhCore_TEST.exe" >nul
if not errorlevel 1 (echo ERROR: AnkhCore_TEST.exe is running ^(a self-test may be writing the journal^). Close it and run again. Nothing was changed.& exit /b 3)

rem --- before: sizes, line counts and hashes of both journals ----------------------------------------------
for /f %%n in ('find /c /v "" ^< "%TARGET%"') do set "BEFORE_LINES=%%n"
for %%f in ("%TARGET%") do set "BEFORE_BYTES=%%~zf"
echo Preview journal now : %BEFORE_LINES% entries, %BEFORE_BYTES% bytes
rem (variables set inside a parenthesised block are NOT visible to an echo in the same block without delayed expansion:
rem  the Stable figures are therefore read by a subroutine, and every echo/compare of them is on its own line)
set "STABLE_LINES=n/a"
set "STABLE_HASH_BEFORE=not present"
if exist "%STABLE%" call :stable_info STABLE_HASH_BEFORE
echo Stable journal now  : %STABLE_LINES% entries, SHA-256 %STABLE_HASH_BEFORE%

rem --- backup name: first unused journal.txt.preview-backup-N (an existing backup is never overwritten) ---------
set /a N=1
:nextname
set "BACKUP=%TARGET_DIR%\journal.txt.preview-backup-%N%"
if exist "%BACKUP%" (set /a N+=1 & goto :nextname)
echo Backup will be       : %BACKUP%

if /i "%MODE%"=="/dry" (
  echo.
  echo DRY RUN: nothing was changed. A real run would copy the journal to the backup, verify it byte for byte,
  echo then delete only "%TARGET%".
  exit /b 0
)

echo.
set /p "ANSWER=Type YES to back up and clean the Preview journal: "
if /i not "%ANSWER%"=="YES" (echo Cancelled. Nothing was changed.& exit /b 1)

copy /b "%TARGET%" "%BACKUP%" >nul
if errorlevel 1 (echo ERROR: the backup could not be written. Nothing was changed.& exit /b 4)
fc /b "%TARGET%" "%BACKUP%" >nul
if errorlevel 1 (echo ERROR: the backup does not match the journal. Nothing was changed; the backup is left for inspection.& exit /b 4)
echo Backup written and verified byte for byte.

del /f /q "%TARGET%"
if exist "%TARGET%" (echo ERROR: the journal could not be deleted. The backup is safe at %BACKUP%.& exit /b 5)

rem --- after ---------------------------------------------------------------------------------------------------
echo.
echo Preview journal is now clean. Backup: %BACKUP%
set "STABLE_HASH_AFTER=not present"
if exist "%STABLE%" call :stable_info STABLE_HASH_AFTER
echo Stable journal after : SHA-256 %STABLE_HASH_AFTER%
if /i "%STABLE_HASH_BEFORE%"=="unreadable" (echo WARNING: the Stable journal could not be hashed, so it could not be verified.& goto :after_done)
if /i "%STABLE_HASH_AFTER%"=="%STABLE_HASH_BEFORE%" (echo Stable journal : unchanged ^(same SHA-256 before and after^).) else (echo WARNING: the Stable journal hash differs from before. It was not touched by this script; check what else wrote it.)
:after_done
echo To undo this clean-up:  preview_journal_cleanup.cmd /restore
exit /b 0

:restore
set "LAST="
for /l %%i in (1,1,500) do if exist "%TARGET_DIR%\journal.txt.preview-backup-%%i" set "LAST=%TARGET_DIR%\journal.txt.preview-backup-%%i"
if not defined LAST (echo No backup found next to the Preview journal. Nothing was changed.& exit /b 1)
echo Newest backup: %LAST%
if exist "%TARGET%" (echo The Preview journal exists now. Restore would overwrite it.)
set /p "ANSWER=Type YES to put this backup back as the Preview journal: "
if /i not "%ANSWER%"=="YES" (echo Cancelled. Nothing was changed.& exit /b 1)
tasklist /fi "imagename eq AnkhCore_TEST.exe" 2>nul | find /i "AnkhCore_TEST.exe" >nul
if not errorlevel 1 (echo ERROR: AnkhCore_TEST.exe is running. Close it and run again.& exit /b 3)
copy /b /y "%LAST%" "%TARGET%" >nul
if errorlevel 1 (echo ERROR: the restore failed.& exit /b 4)
echo Restored. The backup file was kept.
exit /b 0

rem --- read-only helper: entry count of the Stable journal and its SHA-256 (the variable named in %1 receives the hash) ---
:stable_info
set "STABLE_LINES=n/a"
for /f %%n in ('find /c /v "" ^< "%STABLE%"') do set "STABLE_LINES=%%n"
set "%~1="
for /f "tokens=1" %%h in ('certutil -hashfile "%STABLE%" SHA256 ^| findstr /r /x /c:"[0-9a-fA-F][0-9a-fA-F]*"') do if not defined %~1 set "%~1=%%h"
if not defined %~1 set "%~1=unreadable"
exit /b 0
