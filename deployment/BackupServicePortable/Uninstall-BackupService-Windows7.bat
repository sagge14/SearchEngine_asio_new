@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "SERVICE_NAME=SearchEngineBackupService"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "PACKAGE_ROOT=%~dp0"
set "DATA_DIR=%ProgramData%\SearchEngineBackupService"
set "BACKUP_MODE=none"

if /I "%TARGET_ARCH%"=="x86" goto :SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramW6432%"
if "%PROGRAM_ROOT%"=="" set "PROGRAM_ROOT=%ProgramFiles%"
goto :ROOT_READY
:SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramFiles%"
if not "%ProgramFiles(x86)%"=="" set "PROGRAM_ROOT=%ProgramFiles(x86)%"
:ROOT_READY
set "INSTALL_ROOT=%PROGRAM_ROOT%\SearchEngineBackupService"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Uninstall-BackupService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if not errorlevel 1 set "SERVICE_EXISTS=1"
if exist "%INSTALL_ROOT%" set "FILES_EXIST=1"
if exist "%DATA_DIR%" set "FILES_EXIST=1"
if not defined SERVICE_EXISTS if not defined FILES_EXIST goto :NOT_INSTALLED

call :CHOOSE_BACKUP
if errorlevel 1 goto :CANCELLED
echo.
echo This will remove the Windows service, application files, Backup.json
echo and logs from this computer.
echo Snapshot and mirror-history directories from backup_dir are NOT deleted.
echo   1 - Continue uninstall
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED

if not defined SERVICE_EXISTS goto :BACKUP
echo Stopping %SERVICE_NAME%...
call :STOP_SERVICE
if errorlevel 1 goto :FAILED

:BACKUP
if "%BACKUP_MODE%"=="none" goto :DELETE_SERVICE
echo Exporting settings and logs...
call :EXPORT_SETTINGS_LOGS
if errorlevel 1 (
    echo ERROR: Backup failed. Nothing was deleted.
    if defined SERVICE_EXISTS sc.exe start "%SERVICE_NAME%" >nul 2>&1
    goto :FAILED
)

:DELETE_SERVICE
if not defined SERVICE_EXISTS goto :DELETE_FILES
sc.exe delete "%SERVICE_NAME%" >nul
if errorlevel 1 (
    echo ERROR: Cannot delete service registration. Files were preserved.
    goto :FAILED
)

:DELETE_FILES
cd /d "%TEMP%"
rmdir /S /Q "%INSTALL_ROOT%" >nul 2>&1
if exist "%INSTALL_ROOT%" goto :APP_DIR_DELETE_FAILED
rmdir /S /Q "%DATA_DIR%" >nul 2>&1
if exist "%DATA_DIR%" goto :DATA_DIR_DELETE_FAILED

echo.
echo SearchEngineBackupService was completely removed.
echo Snapshot directories under backup_dir were left untouched.
if not "%BACKUP_MODE%"=="none" echo The requested backup was created before deletion.
pause
exit /b 0

:APP_DIR_DELETE_FAILED
echo ERROR: Application directory could not be deleted:
echo   %INSTALL_ROOT%
goto :FAILED
:DATA_DIR_DELETE_FAILED
echo ERROR: Data directory could not be deleted:
echo   %DATA_DIR%
goto :FAILED

:CHOOSE_BACKUP
echo Backup before uninstall:
echo   1 - Settings and logs ^(recommended^)
echo   2 - Do not create a backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CONFIRM_NO_BACKUP
set "BACKUP_MODE=settings-logs"
:READ_BACKUP_DESTINATION
set "BACKUP_DESTINATION="
set /p "BACKUP_DESTINATION=Destination disk or folder, for example E:\Backups: "
if "%BACKUP_DESTINATION%"=="" goto :READ_BACKUP_DESTINATION
exit /b 0
:CONFIRM_NO_BACKUP
echo.
echo WARNING: no backup will be available after deletion.
echo   1 - Cancel ^(recommended^)
echo   2 - Continue without backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 0
exit /b 1

:EXPORT_SETTINGS_LOGS
set "STAMP="
for /f "tokens=1-3 delims=/. " %%A in ("%DATE%") do set "STAMP=%%C%%B%%A"
for /f "tokens=1-2 delims=:." %%A in ("%TIME%") do set "STAMP=%STAMP%-%%A%%B"
set "STAMP=%STAMP: =0%"
set "EXPORT_ROOT=%BACKUP_DESTINATION%\SearchEngineBackupService-uninstall-%STAMP%"
md "%EXPORT_ROOT%" >nul 2>&1
if not exist "%EXPORT_ROOT%\" exit /b 1
if exist "%DATA_DIR%\Backup.json" copy /Y "%DATA_DIR%\Backup.json" "%EXPORT_ROOT%\Backup.json" >nul
if exist "%DATA_DIR%\logs\" xcopy.exe "%DATA_DIR%\logs\*" "%EXPORT_ROOT%\logs\" /E /I /H /R /Y >nul
if exist "%INSTALL_ROOT%\README.txt" copy /Y "%INSTALL_ROOT%\README.txt" "%EXPORT_ROOT%\README.txt" >nul
echo Backup exported to: %EXPORT_ROOT%
exit /b 0

:STOP_SERVICE
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0
:WAIT_STOPPED
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 exit /b 0
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 goto :OFFER_FORCE_STOP
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED
:OFFER_FORCE_STOP
echo The service did not stop within 120 seconds.
echo   1 - Force-terminate its process and continue
echo   2 - Cancel without deleting files
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
set "SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /I "PID"') do set "SERVICE_PID=%%P"
set "SERVICE_PID=%SERVICE_PID: =%"
echo(%SERVICE_PID%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 exit /b 1
taskkill.exe /PID %SERVICE_PID% /T /F >nul 2>&1
if errorlevel 1 exit /b 1
ping.exe 127.0.0.1 -n 3 >nul
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if errorlevel 1 exit /b 1
exit /b 0

:NOT_INSTALLED
echo SearchEngineBackupService and its files are not installed.
pause
exit /b 0
:CANCELLED
echo Uninstall cancelled. Nothing was deleted.
exit /b 1
:FAILED
echo Uninstall failed. Undeleted files were preserved.
pause
exit /b 1
