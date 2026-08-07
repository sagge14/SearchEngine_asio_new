@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "SERVICE_NAME=SearchEngineBackupService"
set "DATA_DIR=%ProgramData%\SearchEngineBackupService"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Restart-BackupService.bat as Administrator.
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
echo SearchEngineBackupService is RUNNING.
echo Logs: %DATA_DIR%\logs
pause
exit /b 0

:OFFER_FORCE_STOP
echo The service did not stop within 120 seconds.
echo   1 - Force-terminate its process and continue
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :STOP_FAILED
set "SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /I "PID"') do set "SERVICE_PID=%%P"
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
echo Check logs in %DATA_DIR%\logs
pause
exit /b 1
