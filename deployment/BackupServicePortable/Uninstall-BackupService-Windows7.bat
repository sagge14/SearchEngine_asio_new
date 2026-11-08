@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "SERVICE_INSTANCE=default"
if exist "%PACKAGE_ROOT%ServiceInstance.cmd" call "%PACKAGE_ROOT%ServiceInstance.cmd"
set "STOP_MODE="
set "ARG1=%~1"
set "ARG2=%~2"

if /I "%ARG1%"=="graceful" (
    set "STOP_MODE=Graceful"
) else if /I "%ARG1%"=="immediate" (
    set "STOP_MODE=Immediate"
) else if not "%ARG1%"=="" (
    set "SERVICE_INSTANCE=%ARG1%"
    if /I "%ARG2%"=="graceful" set "STOP_MODE=Graceful"
    if /I "%ARG2%"=="immediate" set "STOP_MODE=Immediate"
)

echo(%SERVICE_INSTANCE%| findstr.exe /R /X "[A-Za-z0-9][A-Za-z0-9_-]*" >nul
if errorlevel 1 (
    echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
if not "%SERVICE_INSTANCE:~32,1%"=="" (
    echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
if /I "%SERVICE_INSTANCE%"=="default" (
    set "SERVICE_NAME=SearchEngineBackupService"
) else (
    set "SERVICE_NAME=SearchEngineBackupService-%SERVICE_INSTANCE%"
)

set "TARGET_ARCH={{ARCHITECTURE}}"
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "UNINSTALL_LOG=%TEMP%\%SERVICE_NAME%-Uninstall-last.log"
set "UNINSTALL_STAGE=initialization"
set "BACKUP_MODE=none"
set "STOP_TIMEOUT_SECONDS=1800"
set "IMMEDIATE_GRACE_SECONDS=2"
> "%UNINSTALL_LOG%" echo %SERVICE_NAME% uninstall diagnostic log
>> "%UNINSTALL_LOG%" echo Started: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Package: %PACKAGE_ROOT%
>> "%UNINSTALL_LOG%" echo Instance: %SERVICE_INSTANCE%
>> "%UNINSTALL_LOG%" echo StopMode: %STOP_MODE%

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Uninstall-BackupService.bat as Administrator.
    echo Log: %UNINSTALL_LOG%
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

if /I "%TARGET_ARCH%"=="x86" goto :SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramW6432%"
if "%PROGRAM_ROOT%"=="" set "PROGRAM_ROOT=%ProgramFiles%"
goto :ROOT_READY
:SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramFiles%"
if not "%ProgramFiles(x86)%"=="" set "PROGRAM_ROOT=%ProgramFiles(x86)%"
:ROOT_READY
REM Portable layout keeps a single application root; named instances differ by service/data name only.
set "INSTALL_ROOT=%PROGRAM_ROOT%\SearchEngineBackupService"
set "UNINSTALL_STAGE=target selected"
>> "%UNINSTALL_LOG%" echo Service: %SERVICE_NAME%
>> "%UNINSTALL_LOG%" echo Application: %INSTALL_ROOT%
>> "%UNINSTALL_LOG%" echo Data: %DATA_DIR%

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
echo Snapshot and mirror-history folders from backup_dir are NOT deleted.
echo   1 - Continue uninstall
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED

if not defined SERVICE_EXISTS goto :BACKUP
if defined STOP_MODE goto :STOP_MODE_READY
call :RESOLVE_STOP_MODE
if errorlevel 1 goto :CANCELLED
:STOP_MODE_READY
>> "%UNINSTALL_LOG%" echo StopMode selected: %STOP_MODE%
echo Selected StopMode=%STOP_MODE% for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^)
set "UNINSTALL_STAGE=stopping service"
>> "%UNINSTALL_LOG%" echo Stage: stopping service
echo Stopping %SERVICE_NAME% ^(StopMode=%STOP_MODE%^)...
call :STOP_SERVICE
if errorlevel 1 goto :FAILED
>> "%UNINSTALL_LOG%" echo Stage: service stopped

:BACKUP
if "%BACKUP_MODE%"=="none" goto :DELETE_FILES
echo Exporting settings and logs...
set "UNINSTALL_STAGE=creating backup"
>> "%UNINSTALL_LOG%" echo Stage: creating backup
call :EXPORT_SETTINGS_LOGS
if errorlevel 1 goto :BACKUP_FAILED

:DELETE_FILES
cd /d "%TEMP%"
set "UNINSTALL_STAGE=deleting application directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting application directory
echo Deleting application directory...
call :DELETE_DIRECTORY_RETRY "%INSTALL_ROOT%"
if errorlevel 1 goto :APP_DIR_DELETE_FAILED
set "UNINSTALL_STAGE=deleting data directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting data directory
echo Deleting data directory...
call :DELETE_DIRECTORY_RETRY "%DATA_DIR%"
if errorlevel 1 goto :DATA_DIR_DELETE_FAILED
set "UNINSTALL_STAGE=deleting rollback directories"
>> "%UNINSTALL_LOG%" echo Stage: deleting rollback directories
echo Deleting rollback directories...
call :DELETE_ROLLBACK_DIRECTORIES "%INSTALL_ROOT%.rollback-*"
if errorlevel 1 goto :APP_ROLLBACK_DELETE_FAILED
call :DELETE_ROLLBACK_DIRECTORIES "%DATA_DIR%.rollback-*"
if errorlevel 1 goto :DATA_ROLLBACK_DELETE_FAILED

if not defined SERVICE_EXISTS goto :UNINSTALL_DONE
set "UNINSTALL_STAGE=deleting service registration"
>> "%UNINSTALL_LOG%" echo Stage: deleting service registration
echo Deleting service registration...
call :DELETE_SERVICE_RETRY
if errorlevel 1 goto :SERVICE_DELETE_FAILED

:UNINSTALL_DONE
echo.
echo SearchEngineBackupService was completely removed.
echo Snapshot folders under backup_dir were left untouched.
if not "%BACKUP_MODE%"=="none" echo The requested backup was created before deletion.
>> "%UNINSTALL_LOG%" echo Completed successfully: %DATE% %TIME%
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 0

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
>> "%UNINSTALL_LOG%" echo Backup: %EXPORT_ROOT%
exit /b 0

:RESOLVE_STOP_MODE
echo.
echo Stop mode for instance %SERVICE_INSTANCE% ^(service %SERVICE_NAME%^):
echo   [1] Stop gracefully - wait for current tasks to finish (recommended)
echo   [2] Stop immediately - interrupt the backup in progress
echo   [0] Cancel
echo WARNING: Immediate stop interrupts an in-progress backup; unpublished staging may remain until the next start cleans .partial_* directories.
set "CHOICE="
set /p "CHOICE=Select [1]: "
if "%CHOICE%"=="" set "CHOICE=1"
if "%CHOICE%"=="1" (
    set "STOP_MODE=Graceful"
    exit /b 0
)
if "%CHOICE%"=="2" (
    set "STOP_MODE=Immediate"
    exit /b 0
)
if "%CHOICE%"=="0" exit /b 1
echo ERROR: Invalid stop mode choice "%CHOICE%" for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^).
exit /b 1

:VALIDATE_PID
set "CHECK_PID=%~1"
echo(%CHECK_PID%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 exit /b 1
if "%CHECK_PID%"=="0" exit /b 1
if "%CHECK_PID%"=="4" exit /b 1
if %CHECK_PID% LEQ 4 exit /b 1
exit /b 0

:STOP_SERVICE
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 exit /b 0

set "STOPPED_SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "STOPPED_SERVICE_PID=%%P"
set "STOPPED_SERVICE_PID=%STOPPED_SERVICE_PID: =%"
call :VALIDATE_PID "%STOPPED_SERVICE_PID%"
if errorlevel 1 set "STOPPED_SERVICE_PID="

if /I "%STOP_MODE%"=="Immediate" goto :STOP_SERVICE_IMMEDIATE

echo Stopping %SERVICE_NAME% ^(StopMode=Graceful, timeout %STOP_TIMEOUT_SECONDS%s^)...
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0
:WAIT_STOPPED_GRACEFUL
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% (
    echo ERROR: Service %SERVICE_NAME% ^(StopMode=Graceful^) did not reach STOPPED within %STOP_TIMEOUT_SECONDS% seconds.
    exit /b 1
)
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_GRACEFUL

:STOP_SERVICE_IMMEDIATE
echo WARNING: Immediate stop for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^) will interrupt an in-progress backup.
call :VALIDATE_PID "%STOPPED_SERVICE_PID%"
if errorlevel 1 (
    echo ERROR: Refusing invalid ProcessId for Immediate stop of %SERVICE_NAME%.
    exit /b 1
)
set "INITIAL_PID=%STOPPED_SERVICE_PID%"
echo Sending STOP to %SERVICE_NAME% ^(StopMode=Immediate, PID %INITIAL_PID%^)...
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a GRACE_LEFT=%IMMEDIATE_GRACE_SECONDS%
:UNINSTALL_IMMEDIATE_GRACE_WAIT
if %GRACE_LEFT% LEQ 0 goto :UNINSTALL_IMMEDIATE_AFTER_GRACE
ping.exe 127.0.0.1 -n 2 >nul
set /a GRACE_LEFT-=1
goto :UNINSTALL_IMMEDIATE_GRACE_WAIT

:UNINSTALL_IMMEDIATE_AFTER_GRACE
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT

set "PID_CHECK1="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "PID_CHECK1=%%P"
set "PID_CHECK1=%PID_CHECK1: =%"
call :VALIDATE_PID "%PID_CHECK1%"
if errorlevel 1 exit /b 1
if not "%PID_CHECK1%"=="%INITIAL_PID%" (
    echo ERROR: ProcessId changed from %INITIAL_PID% to %PID_CHECK1%. Force-terminate aborted.
    exit /b 1
)

set "PID_CHECK2="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "PID_CHECK2=%%P"
set "PID_CHECK2=%PID_CHECK2: =%"
call :VALIDATE_PID "%PID_CHECK2%"
if errorlevel 1 exit /b 1
if not "%PID_CHECK2%"=="%PID_CHECK1%" (
    echo ERROR: ProcessId changed from %PID_CHECK1% to %PID_CHECK2% between verification queries.
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT

echo Force-terminating verified PID %PID_CHECK2% for %SERVICE_NAME% ^(StopMode=Immediate^)...
taskkill.exe /PID %PID_CHECK2% /F >nul 2>&1
if errorlevel 1 exit /b 1
set "STOPPED_SERVICE_PID=%PID_CHECK2%"
set /a WAIT_SECONDS=0
:WAIT_STOPPED_IMMEDIATE
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% exit /b 1
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_IMMEDIATE

:WAIT_STOPPED_PROCESS_EXIT
if not defined STOPPED_SERVICE_PID exit /b 0
call :VALIDATE_PID "%STOPPED_SERVICE_PID%"
if errorlevel 1 exit /b 0
set /a PROCESS_WAIT_SECONDS=0
:WAIT_STOPPED_PROCESS_LOOP
tasklist.exe /FI "PID eq %STOPPED_SERVICE_PID%" /NH 2>nul | findstr.exe /R /C:"[ ]%STOPPED_SERVICE_PID%[ ]" >nul
if errorlevel 1 exit /b 0
set /a PROCESS_WAIT_SECONDS+=1
if %PROCESS_WAIT_SECONDS% GEQ 30 (
    echo ERROR: Service is STOPPED, but PID %STOPPED_SERVICE_PID% is still running.
    exit /b 1
)
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_PROCESS_LOOP

:DELETE_DIRECTORY_RETRY
set "DELETE_TARGET=%~1"
if not exist "%DELETE_TARGET%\" exit /b 0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
set /a DELETE_ATTEMPT=0
:DELETE_DIRECTORY_LOOP
rmdir /S /Q "%DELETE_TARGET%" >nul 2>&1
if not exist "%DELETE_TARGET%\" exit /b 0
set /a DELETE_ATTEMPT+=1
if %DELETE_ATTEMPT% GEQ 30 goto :OFFER_DIRECTORY_DELETE_RETRY
ping.exe 127.0.0.1 -n 2 >nul
goto :DELETE_DIRECTORY_LOOP

:OFFER_DIRECTORY_DELETE_RETRY
echo.
echo Directory is still in use or access is denied:
echo   %DELETE_TARGET%
echo Close Explorer, Total Commander, database tools and other programs that
echo may have this directory open.
echo   1 - Retry deletion
echo   2 - Cancel and preserve the remaining files
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
set /a DELETE_ATTEMPT=0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
goto :DELETE_DIRECTORY_LOOP

:DELETE_SERVICE_RETRY
sc.exe delete "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
set /a DELETE_SERVICE_ATTEMPT=0
:DELETE_SERVICE_LOOP
reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 exit /b 0
set /a DELETE_SERVICE_ATTEMPT+=1
if %DELETE_SERVICE_ATTEMPT% GEQ 30 goto :OFFER_SERVICE_DELETE_RETRY
ping.exe 127.0.0.1 -n 2 >nul
goto :DELETE_SERVICE_LOOP

:OFFER_SERVICE_DELETE_RETRY
echo.
echo Windows still keeps the service registration open:
echo   %SERVICE_NAME%
echo Close the Services window ^(services.msc^), Event Viewer and other
echo management consoles, then retry.
echo   1 - Retry the registration check
echo   2 - Cancel; the service remains stopped
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
sc.exe delete "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
set /a DELETE_SERVICE_ATTEMPT=0
goto :DELETE_SERVICE_LOOP

:DELETE_ROLLBACK_DIRECTORIES
set "ROLLBACK_DELETE_FAILED="
for /D %%D in ("%~1") do call :DELETE_ONE_ROLLBACK_DIRECTORY "%%~fD"
if defined ROLLBACK_DELETE_FAILED exit /b 1
exit /b 0

:DELETE_ONE_ROLLBACK_DIRECTORY
call :DELETE_DIRECTORY_RETRY "%~1"
if errorlevel 1 set "ROLLBACK_DELETE_FAILED=1"
exit /b 0

:NOT_INSTALLED
echo SearchEngineBackupService and its files are not installed.
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 0
:CANCELLED
>> "%UNINSTALL_LOG%" echo Cancelled: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Stage: %UNINSTALL_STAGE%
echo Uninstall cancelled. Nothing was deleted.
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 1
:BACKUP_FAILED
echo ERROR: Backup failed. Nothing was deleted.
if defined SERVICE_EXISTS sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED
:APP_DIR_DELETE_FAILED
echo ERROR: Application directory could not be deleted:
echo   %INSTALL_ROOT%
goto :FAILED
:DATA_DIR_DELETE_FAILED
echo ERROR: Data directory could not be deleted:
echo   %DATA_DIR%
goto :FAILED
:APP_ROLLBACK_DELETE_FAILED
echo ERROR: An application rollback directory could not be deleted.
goto :FAILED
:DATA_ROLLBACK_DELETE_FAILED
echo ERROR: A data rollback directory could not be deleted.
goto :FAILED
:SERVICE_DELETE_FAILED
echo ERROR: Files were deleted, but the service registration remains.
echo Run this uninstaller again for %SERVICE_NAME%.
goto :FAILED
:FAILED
call :WRITE_FAILURE_DIAGNOSTICS
echo Uninstall failed. Undeleted files were preserved.
echo Failed stage: %UNINSTALL_STAGE%
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 1

:WRITE_FAILURE_DIAGNOSTICS
>> "%UNINSTALL_LOG%" echo.
>> "%UNINSTALL_LOG%" echo Failed: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Stage: %UNINSTALL_STAGE%
>> "%UNINSTALL_LOG%" echo Service query:
sc.exe queryex "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
>> "%UNINSTALL_LOG%" echo Service registry key:
reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
>> "%UNINSTALL_LOG%" echo Application directory:
if exist "%INSTALL_ROOT%\" dir /A "%INSTALL_ROOT%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%INSTALL_ROOT%\" >> "%UNINSTALL_LOG%" echo MISSING
>> "%UNINSTALL_LOG%" echo Data directory:
if exist "%DATA_DIR%\" dir /A "%DATA_DIR%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%DATA_DIR%\" >> "%UNINSTALL_LOG%" echo MISSING
exit /b 0

:WAIT_BEFORE_CLOSE
echo.
choice.exe /C 0 /N /M "Press 0 to close: "
exit /b 0
