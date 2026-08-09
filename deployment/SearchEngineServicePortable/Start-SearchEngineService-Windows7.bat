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
    set "DISPLAY_NAME=Search Engine ASIO Server"
) else (
    set "SERVICE_NAME=SearchEngineService-%SERVICE_INSTANCE%"
    set "DISPLAY_NAME=Search Engine ASIO Server (%SERVICE_INSTANCE%)"
)
set "FIREWALL_RULE=%SERVICE_NAME% TCP"
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "HELPER_OUTPUT=%TEMP%\%SERVICE_NAME%-Start-%RANDOM%-%RANDOM%.txt"
set "START_TIMEOUT_SECONDS=1800"
set "ENDPOINT_PORT="
set "FIREWALL_PORT="

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Start-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% is not installed.
    pause
    exit /b 1
)

set "SERVICE_PORT="
"%HELPER%" inspect --settings "%DATA_DIR%\Settings.json" > "%HELPER_OUTPUT%" 2>nul
if errorlevel 1 goto :INVALID_SETTINGS
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do if /I "%%A"=="port" set "SERVICE_PORT=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined SERVICE_PORT goto :INVALID_SETTINGS
echo(%SERVICE_PORT%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 goto :INVALID_SETTINGS

echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Runtime Settings.json: %DATA_DIR%\Settings.json
echo ASIO port from installed Settings.json: %SERVICE_PORT%
call :WARN_PORT_MISMATCH

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :HEALTH_ONLY

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]2[ ]*START_PENDING" >nul
if not errorlevel 1 goto :WAIT_RUNNING

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]3[ ]*STOP_PENDING" >nul
if not errorlevel 1 goto :WAIT_STOPPED_THEN_START

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if errorlevel 1 goto :UNSUPPORTED_STATE

echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :START_FAILED
goto :WAIT_RUNNING

:WAIT_STOPPED_THEN_START
echo Service is STOP_PENDING; waiting for STOPPED before start...
set /a WAIT_SECONDS=0
:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :START_AFTER_STOP
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ %START_TIMEOUT_SECONDS% goto :START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP

:START_AFTER_STOP
echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :START_FAILED

:WAIT_RUNNING
echo Waiting for RUNNING state...
set /a WAIT_SECONDS=0
:WAIT_RUNNING_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :HEALTH_CHECK
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for RUNNING... %WAIT_SECONDS%s / %START_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %START_TIMEOUT_SECONDS% goto :START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING_LOOP

:HEALTH_ONLY
echo %SERVICE_NAME% is already RUNNING; checking PING/PONG...

:HEALTH_CHECK
"%HELPER%" health --port %SERVICE_PORT% --timeout-ms 10000 >nul 2>&1
if errorlevel 1 goto :HEALTH_FAILED
echo %SERVICE_NAME% is RUNNING and answers PING/PONG on port %SERVICE_PORT%.
echo Logs: %DATA_DIR%\logs
echo Stop -^> Start creates a new process and re-reads Settings.json.
pause
exit /b 0

:WARN_PORT_MISMATCH
if exist "%DATA_DIR%\client-endpoint.txt" goto :WARN_READ_ENDPOINT
echo WARNING: client-endpoint.txt was not found under %DATA_DIR%.
echo After a port change, update client settings manually.
goto :WARN_CHECK_FIREWALL

:WARN_READ_ENDPOINT
for /f "usebackq tokens=1,* delims==" %%A in ("%DATA_DIR%\client-endpoint.txt") do if /I "%%A"=="port" set "ENDPOINT_PORT=%%B"
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

:START_FAILED
echo ERROR: Service did not reach RUNNING state.
sc.exe query "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:HEALTH_FAILED
echo ERROR: Service is RUNNING but did not answer PING within 10 seconds on port %SERVICE_PORT%.
sc.exe query "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
echo Checked port comes from %DATA_DIR%\Settings.json, not from the portable data\Settings.json template.
pause
exit /b 1

:UNSUPPORTED_STATE
echo ERROR: Service %SERVICE_NAME% is in an unsupported transitional state.
echo Pause/Continue is not supported by SearchEngineService. Use Stop then Start.
sc.exe query "%SERVICE_NAME%"
echo Logs: %DATA_DIR%\logs
pause
exit /b 1

:INVALID_SETTINGS
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo ERROR: Installed Settings.json is missing or invalid:
echo   %DATA_DIR%\Settings.json
echo The portable package data\Settings.json template is not used at runtime.
pause
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1
