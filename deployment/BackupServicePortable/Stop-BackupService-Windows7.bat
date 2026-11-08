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
if errorlevel 1 goto :INVALID_INSTANCE
if not "%SERVICE_INSTANCE:~32,1%"=="" goto :INVALID_INSTANCE
if /I "%SERVICE_INSTANCE%"=="default" (
    set "SERVICE_NAME=SearchEngineBackupService"
) else (
    set "SERVICE_NAME=SearchEngineBackupService-%SERVICE_INSTANCE%"
)
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "STOP_TIMEOUT_SECONDS=1800"
set "IMMEDIATE_GRACE_SECONDS=2"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Stop-BackupService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^) is not installed.
    pause
    exit /b 1
)

echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Installed config: %DATA_DIR%\Backup.json

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 (
    echo %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^) is already STOPPED.
    echo Logs: %DATA_DIR%\logs
    echo Note: Automatic / Delayed Start services will start again after reboot.
    echo For lasting disable, set Startup Type to Manual or Disabled separately.
    pause
    exit /b 0
)

if defined STOP_MODE goto :HAVE_STOP_MODE
call :RESOLVE_STOP_MODE
if errorlevel 1 goto :CANCELLED

:HAVE_STOP_MODE
echo Selected StopMode=%STOP_MODE% for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^)
if /I "%STOP_MODE%"=="Immediate" goto :STOP_IMMEDIATE
goto :STOP_GRACEFUL

:STOP_GRACEFUL
echo Stopping %SERVICE_NAME% ^(StopMode=Graceful^)...
echo Waiting up to %STOP_TIMEOUT_SECONDS% seconds for a graceful STOPPED state.
echo A long-running backup operation may delay STOPPED.
echo This script does not force-terminate the process in Graceful mode.
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]3[ ]*STOP_PENDING" >nul
if not errorlevel 1 goto :WAIT_STOPPED
sc.exe stop "%SERVICE_NAME%"
if errorlevel 1 (
    sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
    if not errorlevel 1 goto :SUCCESS
    sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]3[ ]*STOP_PENDING" >nul
    if errorlevel 1 goto :STOP_COMMAND_FAILED
)

:WAIT_STOPPED
set /a WAIT_SECONDS=0

:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :SUCCESS
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for STOPPED... %WAIT_SECONDS%s / %STOP_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% goto :STOP_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP

:STOP_IMMEDIATE
echo WARNING: Immediate stop for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^) will interrupt an in-progress backup.
echo Unpublished staging ^(.partial_*^) may remain until the next service start.
echo Published snapshots, cache and configuration are not deleted.
set "INITIAL_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "INITIAL_PID=%%P"
set "INITIAL_PID=%INITIAL_PID: =%"
call :VALIDATE_PID "%INITIAL_PID%"
if errorlevel 1 goto :INVALID_PID

echo Sending STOP to %SERVICE_NAME% ^(StopMode=Immediate, PID %INITIAL_PID%^)...
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a GRACE_LEFT=%IMMEDIATE_GRACE_SECONDS%
:IMMEDIATE_GRACE_WAIT
if %GRACE_LEFT% LEQ 0 goto :IMMEDIATE_AFTER_GRACE
ping.exe 127.0.0.1 -n 2 >nul
set /a GRACE_LEFT-=1
goto :IMMEDIATE_GRACE_WAIT

:IMMEDIATE_AFTER_GRACE
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 (
    echo %SERVICE_NAME% reached STOPPED without force-terminate.
    goto :SUCCESS
)

set "PID_CHECK1="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "PID_CHECK1=%%P"
set "PID_CHECK1=%PID_CHECK1: =%"
call :VALIDATE_PID "%PID_CHECK1%"
if errorlevel 1 goto :INVALID_PID
if not "%PID_CHECK1%"=="%INITIAL_PID%" (
    echo ERROR: ProcessId changed from %INITIAL_PID% to %PID_CHECK1% for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=Immediate^).
    echo Force-terminate aborted.
    pause
    exit /b 1
)

set "PID_CHECK2="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "PID_CHECK2=%%P"
set "PID_CHECK2=%PID_CHECK2: =%"
call :VALIDATE_PID "%PID_CHECK2%"
if errorlevel 1 goto :INVALID_PID
if not "%PID_CHECK2%"=="%PID_CHECK1%" (
    echo ERROR: ProcessId changed from %PID_CHECK1% to %PID_CHECK2% between verification queries.
    echo Force-terminate aborted for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=Immediate^).
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 (
    echo %SERVICE_NAME% reached STOPPED before force-terminate.
    goto :SUCCESS
)

echo Force-terminating verified PID %PID_CHECK2% for %SERVICE_NAME% ^(StopMode=Immediate^)...
taskkill.exe /PID %PID_CHECK2% /F >nul 2>&1
if errorlevel 1 (
    echo ERROR: taskkill failed for verified PID %PID_CHECK2% of %SERVICE_NAME%.
    sc.exe queryex "%SERVICE_NAME%"
    echo Logs: %DATA_DIR%\logs
    pause
    exit /b 1
)

set /a WAIT_SECONDS=0
:WAIT_IMMEDIATE_STOPPED
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :IMMEDIATE_SUCCESS
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% goto :STOP_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_IMMEDIATE_STOPPED

:IMMEDIATE_SUCCESS
echo WARNING: An interrupted backup may leave .partial_* staging until the next start.
goto :SUCCESS

:SUCCESS
echo %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=%STOP_MODE%^) is STOPPED.
echo Logs: %DATA_DIR%\logs
echo Note: Automatic / Delayed Start services will start again after reboot.
echo For lasting disable, set Startup Type to Manual or Disabled separately.
echo Stop -^> Start creates a new process and re-reads the installed Backup.json.
pause
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

:INVALID_PID
echo ERROR: Refusing invalid or unverified ProcessId for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=Immediate^).
sc.exe queryex "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:CANCELLED
echo Stop cancelled by user for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%^). Service state was not changed.
pause
exit /b 1

:STOP_COMMAND_FAILED
echo ERROR: sc.exe stop failed for %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=%STOP_MODE%^).
sc.exe queryex "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:STOP_FAILED
echo ERROR: Service %SERVICE_NAME% ^(InstanceId=%SERVICE_INSTANCE%, StopMode=%STOP_MODE%^) did not reach STOPPED within %STOP_TIMEOUT_SECONDS% seconds.
sc.exe queryex "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1
