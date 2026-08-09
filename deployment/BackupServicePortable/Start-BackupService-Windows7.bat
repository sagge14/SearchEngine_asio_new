@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "SERVICE_INSTANCE=default"
if exist "%PACKAGE_ROOT%ServiceInstance.cmd" call "%PACKAGE_ROOT%ServiceInstance.cmd"
if not "%~1"=="" set "SERVICE_INSTANCE=%~1"
echo(%SERVICE_INSTANCE%| findstr.exe /R /X "[A-Za-z0-9][A-Za-z0-9_-]*" >nul
if errorlevel 1 goto :INVALID_INSTANCE
if not "%SERVICE_INSTANCE:~32,1%"=="" goto :INVALID_INSTANCE
if /I "%SERVICE_INSTANCE%"=="default" (
    set "SERVICE_NAME=SearchEngineBackupService"
) else (
    set "SERVICE_NAME=SearchEngineBackupService-%SERVICE_INSTANCE%"
)
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "START_TIMEOUT_SECONDS=120"
set "STOP_PENDING_TIMEOUT_SECONDS=1800"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Start-BackupService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% is not installed.
    pause
    exit /b 1
)

echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Installed Backup.json: %DATA_DIR%\Backup.json
echo The portable package data\Backup.json template is not used at runtime.

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :ALREADY_RUNNING

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]2[ ]*START_PENDING" >nul
if not errorlevel 1 goto :WAIT_RUNNING

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]3[ ]*STOP_PENDING" >nul
if not errorlevel 1 goto :WAIT_STOPPED_THEN_START

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if errorlevel 1 goto :UNSUPPORTED_STATE

echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%"
if errorlevel 1 goto :START_FAILED
goto :WAIT_RUNNING

:WAIT_STOPPED_THEN_START
echo Service is STOP_PENDING; waiting for STOPPED before start...
set /a WAIT_SECONDS=0
:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :START_AFTER_STOP
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for STOPPED... %WAIT_SECONDS%s / %STOP_PENDING_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %STOP_PENDING_TIMEOUT_SECONDS% goto :START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP

:START_AFTER_STOP
echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%"
if errorlevel 1 goto :START_FAILED

:WAIT_RUNNING
echo Waiting for RUNNING state...
set /a WAIT_SECONDS=0
:WAIT_RUNNING_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :SUCCESS
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for RUNNING... %WAIT_SECONDS%s / %START_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %START_TIMEOUT_SECONDS% goto :START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING_LOOP

:ALREADY_RUNNING
echo %SERVICE_NAME% is already RUNNING.
echo Logs: %DATA_DIR%\logs
pause
exit /b 0

:SUCCESS
echo %SERVICE_NAME% is RUNNING.
echo Logs: %DATA_DIR%\logs
echo The new process re-read the installed Backup.json.
echo Installed config: %DATA_DIR%\Backup.json
echo Stop -^> Start creates a new process; edit ProgramData Backup.json, not the portable template.
pause
exit /b 0

:START_FAILED
echo ERROR: Service did not reach RUNNING state.
sc.exe query "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
echo Checked config path: %DATA_DIR%\Backup.json
echo The portable package data\Backup.json template is not used at runtime.
pause
exit /b 1

:UNSUPPORTED_STATE
echo ERROR: Service %SERVICE_NAME% is in an unsupported transitional state.
echo Pause/Continue is not supported by BackupService. Use Stop then Start.
sc.exe query "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1
