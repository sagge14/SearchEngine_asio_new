@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_ROOT=%~dp0"
set "STOP_TIMEOUT_SECONDS=1800"
set "START_TIMEOUT_SECONDS=1800"
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Restart-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)
if not exist "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" goto :PACKAGE_MISSING
call "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" restart "%~1"
if errorlevel 2 goto :CANCELLED
if errorlevel 1 goto :FAILED
set "HELPER_OUTPUT=%TEMP%\SE-Restart-%RANDOM%-%RANDOM%.txt"
set "SERVICE_PORT="
"%HELPER%" inspect --settings "%SETTINGS_PATH%" > "%HELPER_OUTPUT%"
if errorlevel 1 goto :INVALID_SETTINGS
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do if /I "%%A"=="port" set "SERVICE_PORT=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined SERVICE_PORT goto :INVALID_SETTINGS
echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Runtime Settings.json: "%SETTINGS_PATH%"
echo Graceful restart; no forced process termination.
"%HELPER%" service-control --action stop --instance "%SERVICE_INSTANCE%" --timeout-seconds %STOP_TIMEOUT_SECONDS%
if errorlevel 1 goto :FAILED
"%HELPER%" service-control --action start --instance "%SERVICE_INSTANCE%" --timeout-seconds %START_TIMEOUT_SECONDS%
if errorlevel 1 goto :FAILED
"%HELPER%" health --port %SERVICE_PORT% --timeout-ms 10000
if errorlevel 1 goto :HEALTH_FAILED
echo %SERVICE_NAME% is RUNNING and answers PING/PONG on port %SERVICE_PORT%.
echo Logs: "%DATA_DIR%\logs"
pause
exit /b 0
:INVALID_SETTINGS
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo ERROR: Installed Settings.json is missing or invalid: "%SETTINGS_PATH%"
goto :FAILED
:HEALTH_FAILED
echo ERROR: Service is RUNNING but did not answer PING/PONG on port %SERVICE_PORT%.
goto :FAILED
:CANCELLED
pause
exit /b 0
:PACKAGE_MISSING
echo ERROR: Resolve-ServiceInstance.cmd is missing. Use the complete portable folder.
:FAILED
if defined DATA_DIR echo Logs: "%DATA_DIR%\logs"
pause
exit /b 1
