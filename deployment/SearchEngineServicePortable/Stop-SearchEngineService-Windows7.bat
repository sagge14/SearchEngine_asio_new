@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_ROOT=%~dp0"
set "STOP_TIMEOUT_SECONDS=1800"
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Stop-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)
if not exist "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" goto :PACKAGE_MISSING
call "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" stop "%~1"
if errorlevel 2 goto :CANCELLED
if errorlevel 1 goto :FAILED
echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Waiting up to %STOP_TIMEOUT_SECONDS% seconds for a graceful STOPPED state.
echo This script does not force-terminate the process.
"%HELPER%" service-control --action stop --instance "%SERVICE_INSTANCE%" --timeout-seconds %STOP_TIMEOUT_SECONDS%
if errorlevel 1 goto :FAILED
echo Logs: "%DATA_DIR%\logs"
echo Note: Automatic / Delayed Start services will start again after reboot.
echo For lasting disable, set Startup Type to Manual or Disabled separately.
echo Stop -^> Start creates a new process and re-reads Settings.json.
pause
exit /b 0
:CANCELLED
pause
exit /b 0
:PACKAGE_MISSING
echo ERROR: Resolve-ServiceInstance.cmd is missing. Use the complete portable folder.
:FAILED
if defined DATA_DIR echo Logs: "%DATA_DIR%\logs"
pause
exit /b 1
