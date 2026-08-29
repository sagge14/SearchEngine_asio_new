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
    set "SERVICE_NAME=SearchEngineService"
) else (
    set "SERVICE_NAME=SearchEngineService-%SERVICE_INSTANCE%"
)
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "STOP_TIMEOUT_SECONDS=1800"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Stop-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% is not installed.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 (
    echo %SERVICE_NAME% is already STOPPED.
    echo Logs: %DATA_DIR%\logs
    echo Note: Automatic / Delayed Start services will start again after reboot.
    echo For lasting disable, set Startup Type to Manual or Disabled separately.
    pause
    exit /b 0
)

echo Stopping %SERVICE_NAME%...
echo Waiting up to %STOP_TIMEOUT_SECONDS% seconds for a graceful STOPPED state.
echo This script does not force-terminate the process.
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0

:WAIT_STOPPED
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :SUCCESS
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for STOPPED... %WAIT_SECONDS%s / %STOP_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% goto :STOP_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED

:SUCCESS
echo %SERVICE_NAME% is STOPPED.
echo Logs: %DATA_DIR%\logs
echo Note: Automatic / Delayed Start services will start again after reboot.
echo For lasting disable, set Startup Type to Manual or Disabled separately.
echo Stop -^> Start creates a new process and re-reads Settings.json.
pause
exit /b 0

:STOP_FAILED
echo ERROR: Service did not reach STOPPED within %STOP_TIMEOUT_SECONDS% seconds.
echo A large index update or background work may still be closing.
echo This script does not offer forced process termination.
sc.exe queryex "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1
