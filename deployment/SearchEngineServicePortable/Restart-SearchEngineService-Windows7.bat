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
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "HELPER_OUTPUT=%TEMP%\%SERVICE_NAME%-Restart-%RANDOM%-%RANDOM%.txt"

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

set "SERVICE_PORT="
"%HELPER%" inspect --settings "%DATA_DIR%\Settings.json" > "%HELPER_OUTPUT%" 2>nul
if errorlevel 1 goto :INVALID_SETTINGS
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do if /I "%%A"=="port" set "SERVICE_PORT=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined SERVICE_PORT (
    goto :INVALID_SETTINGS
)

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Restart-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% is not installed.
    pause
    exit /b 1
)

echo Stopping %SERVICE_NAME%...
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0

:WAIT_STOPPED
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :START_SERVICE
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 goto :OFFER_FORCE_STOP
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED

:START_SERVICE
echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :START_FAILED
set /a WAIT_SECONDS=0

:WAIT_RUNNING
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :SUCCESS
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 goto :START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING

:SUCCESS
"%HELPER%" health --port %SERVICE_PORT% --timeout-ms 10000 >nul 2>&1
if errorlevel 1 goto :HEALTH_FAILED
echo %SERVICE_NAME% is running and answers PING/PONG.
pause
exit /b 0

:OFFER_FORCE_STOP
echo The service did not stop within 120 seconds.
echo   1 - Force-terminate its process and continue
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :STOP_FAILED
set "SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "SERVICE_PID=%%P"
set "SERVICE_PID=%SERVICE_PID: =%"
echo(%SERVICE_PID%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 goto :STOP_FAILED
taskkill.exe /PID %SERVICE_PID% /T /F >nul 2>&1
if errorlevel 1 goto :STOP_FAILED
ping.exe 127.0.0.1 -n 3 >nul
goto :START_SERVICE

:STOP_FAILED
echo ERROR: Service did not stop within 120 seconds.
pause
exit /b 1

:START_FAILED
echo ERROR: Service did not reach RUNNING state.
sc.exe query "%SERVICE_NAME%"
pause
exit /b 1

:HEALTH_FAILED
echo ERROR: Service is RUNNING but did not answer PING within 10 seconds.
echo Check logs in %DATA_DIR%\logs
pause
exit /b 1

:INVALID_SETTINGS
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo ERROR: Installed Settings.json is missing or invalid.
pause
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1
