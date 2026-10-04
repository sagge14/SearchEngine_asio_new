@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_ROOT=%~dp0"
set "START_TIMEOUT_SECONDS=1800"
set "ENDPOINT_PORT="
set "FIREWALL_PORT="
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Start-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)
if not exist "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" goto :PACKAGE_MISSING
call "%PACKAGE_ROOT%Resolve-ServiceInstance.cmd" start "%~1"
if errorlevel 2 goto :CANCELLED
if errorlevel 1 goto :FAILED
set "DISPLAY_NAME=Search Engine ASIO Server (%SERVICE_INSTANCE%)"
if /I "%SERVICE_INSTANCE%"=="default" set "DISPLAY_NAME=Search Engine ASIO Server"
set "FIREWALL_RULE=%SERVICE_NAME% TCP"
set "HELPER_OUTPUT=%TEMP%\SE-Start-%RANDOM%-%RANDOM%.txt"
set "SERVICE_PORT="
"%HELPER%" inspect --settings "%SETTINGS_PATH%" > "%HELPER_OUTPUT%"
if errorlevel 1 goto :INVALID_SETTINGS
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do if /I "%%A"=="port" set "SERVICE_PORT=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined SERVICE_PORT goto :INVALID_SETTINGS
echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Runtime Settings.json: "%SETTINGS_PATH%"
echo ASIO port from installed Settings.json: %SERVICE_PORT%
call :WARN_PORT_MISMATCH
"%HELPER%" service-control --action start --instance "%SERVICE_INSTANCE%" --timeout-seconds %START_TIMEOUT_SECONDS%
if errorlevel 1 goto :FAILED
"%HELPER%" health --port %SERVICE_PORT% --timeout-ms 10000
if errorlevel 1 goto :HEALTH_FAILED
echo %SERVICE_NAME% is RUNNING and answers PING/PONG on port %SERVICE_PORT%.
echo Logs: "%DATA_DIR%\logs"
echo Stop -^> Start creates a new process and re-reads Settings.json.
pause
exit /b 0

:WARN_PORT_MISMATCH
if exist "%ENDPOINT_PATH%" goto :WARN_READ_ENDPOINT
echo WARNING: client-endpoint.txt was not found under "%DATA_DIR%".
echo After a port change, update client settings manually.
goto :WARN_CHECK_FIREWALL

:WARN_READ_ENDPOINT
for /f "usebackq tokens=1,* delims==" %%A in ("%ENDPOINT_PATH%") do if /I "%%A"=="port" set "ENDPOINT_PORT=%%B"
if not defined ENDPOINT_PORT goto :WARN_CHECK_FIREWALL
if /I "%ENDPOINT_PORT%"=="%SERVICE_PORT%" goto :WARN_CHECK_FIREWALL
echo WARNING: Settings.json port is %SERVICE_PORT%, but client-endpoint.txt still lists port=%ENDPOINT_PORT%.
echo Update the client host/port settings manually. This script does not change the client database.

:WARN_CHECK_FIREWALL
set "FIREWALL_PORT="
for /f "tokens=1,* delims=:" %%A in ('netsh.exe advfirewall firewall show rule name^="%FIREWALL_RULE%" verbose 2^>nul ^| findstr.exe /I /C:"LocalPort"') do set "FIREWALL_PORT=%%B"
if defined FIREWALL_PORT set "FIREWALL_PORT=%FIREWALL_PORT: =%"
if not defined FIREWALL_PORT goto :WARN_MISSING_FIREWALL
if /I "%FIREWALL_PORT%"=="%SERVICE_PORT%" exit /b 0
echo WARNING: Firewall rule "%FIREWALL_RULE%" allows LocalPort=%FIREWALL_PORT%, but Settings.json now uses %SERVICE_PORT%.
echo Recreate the inbound TCP rule for the new port. This script does not modify firewall rules.
exit /b 0

:WARN_MISSING_FIREWALL
netsh.exe advfirewall firewall show rule name="%FIREWALL_RULE%" >nul 2>&1
if not errorlevel 1 exit /b 0
rem Avoid parentheses inside if (...): DISPLAY_NAME may contain "(instance)".
set "PS_FIREWALL_RULE=%DISPLAY_NAME% (%SERVICE_PORT%/TCP)"
netsh.exe advfirewall firewall show rule name="%PS_FIREWALL_RULE%" >nul 2>&1
if not errorlevel 1 exit /b 0
echo WARNING: No matching install firewall rule was found for port %SERVICE_PORT%.
echo If clients connect remotely, verify or recreate the Windows Firewall allow rule manually.
exit /b 0


:INVALID_SETTINGS
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo ERROR: Installed Settings.json is missing or invalid: "%SETTINGS_PATH%"
echo The portable package data\Settings.json template is not used at runtime.
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
