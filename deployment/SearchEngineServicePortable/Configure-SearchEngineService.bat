@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem SVC-001: Safe installed-service configuration workflow.
rem Edits Settings.json of an installed SearchEngineService instance.
rem Reads the actual --data-dir from SCM (not %ProgramData%),
rem validates the edited copy, applies atomically via Stop->Start,
rem confirms PING/PONG, and rolls back automatically on any failure.
rem Requires Administrator elevation.

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
set "FIREWALL_RULE=%SERVICE_NAME% TCP"
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "STOP_TIMEOUT_SECONDS=300"
set "START_TIMEOUT_SECONDS=1800"

rem Unique temp paths (instance + PID + RANDOM)
set "EDIT_TEMP=%TEMP%\SE-Configure-%SERVICE_INSTANCE%-%~z0-%RANDOM%-%RANDOM%-edit.json"
set "ENDPOINT_TEMP=%TEMP%\SE-Configure-%SERVICE_INSTANCE%-%~z0-%RANDOM%-%RANDOM%-endpoint.txt"
set "ROLLBACK_DIR=%TEMP%\SE-Configure-%SERVICE_INSTANCE%-%~z0-%RANDOM%-%RANDOM%-rollback"
set "HELPER_OUTPUT=%TEMP%\SE-Configure-%SERVICE_INSTANCE%-%~z0-%RANDOM%-%RANDOM%.txt"

rem SCM-resolved runtime info
set "DATA_DIR="
set "SETTINGS_PATH="
set "ENDPOINT_PATH="
set "PROGRAM_PATH="

rem Ports before/after edit
set "OLD_PORT="
set "NEW_PORT="
set "OLD_YEAR="
set "NEW_YEAR="

rem Firewall tracking
set "OLD_FIREWALL_PORT="
set "FIREWALL_MODIFIED="

rem Rollback tracking
set "ROLLBACK_APPLIED="
set "ENDPOINT_MANAGED="

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Configure-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Service %SERVICE_NAME% is not installed.
    pause
    exit /b 1
)

rem --- Step 1: Resolve actual data-dir via SCM ---
echo Resolving installed data directory from Service Control Manager...
"%HELPER%" inspect-installed --instance "%SERVICE_INSTANCE%" > "%HELPER_OUTPUT%" 2>&1
if errorlevel 1 (
    type "%HELPER_OUTPUT%"
    del /Q "%HELPER_OUTPUT%" >nul 2>&1
    echo ERROR: Could not resolve data-dir for %SERVICE_NAME%.
    echo Verify that the service has a --data-dir argument in its ImagePath.
    pause
    exit /b 1
)
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do (
    if /I "%%A"=="data_dir"       set "DATA_DIR=%%B"
    if /I "%%A"=="settings_path"  set "SETTINGS_PATH=%%B"
    if /I "%%A"=="endpoint_path"  set "ENDPOINT_PATH=%%B"
    if /I "%%A"=="installed_program_path" set "PROGRAM_PATH=%%B"
)
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined DATA_DIR (
    echo ERROR: inspect-installed did not return data_dir.
    pause
    exit /b 1
)
echo Instance  : %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo Data dir  : %DATA_DIR%
echo Settings  : %SETTINGS_PATH%
echo Endpoint  : %ENDPOINT_PATH%

rem --- Step 2: Inspect current settings (old port/year) ---
"%HELPER%" inspect --settings "%SETTINGS_PATH%" > "%HELPER_OUTPUT%" 2>&1
if errorlevel 1 (
    del /Q "%HELPER_OUTPUT%" >nul 2>&1
    echo ERROR: Current Settings.json is missing or invalid.
    pause
    exit /b 1
)
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do (
    if /I "%%A"=="port" set "OLD_PORT=%%B"
    if /I "%%A"=="year" set "OLD_YEAR=%%B"
)
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo Current port: %OLD_PORT%   year: %OLD_YEAR%

rem --- Step 3: Copy active Settings.json to temp for editing ---
copy /Y "%SETTINGS_PATH%" "%EDIT_TEMP%" >nul
if errorlevel 1 (
    echo ERROR: Cannot copy Settings.json to temp file.
    goto :CLEANUP_TEMPS
)

rem --- Step 4: Open temp file for editing; loop until valid ---
:EDIT_LOOP
notepad.exe "%EDIT_TEMP%"

"%HELPER%" validate --settings "%EDIT_TEMP%" > "%HELPER_OUTPUT%" 2>&1
if not errorlevel 1 goto :EDIT_VALID

echo.
echo === VALIDATION FAILED ===
type "%HELPER_OUTPUT%"
echo ========================
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo.
echo The edited Settings.json is invalid.
choice /C YN /M "Open editor again to fix? [Y=yes, N=cancel]"
if errorlevel 2 goto :CANCELLED
goto :EDIT_LOOP

:EDIT_VALID
del /Q "%HELPER_OUTPUT%" >nul 2>&1

rem --- Step 5: Inspect new port/year ---
"%HELPER%" inspect --settings "%EDIT_TEMP%" > "%HELPER_OUTPUT%" 2>&1
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do (
    if /I "%%A"=="port" set "NEW_PORT=%%B"
    if /I "%%A"=="year" set "NEW_YEAR=%%B"
)
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo.
echo Old port: %OLD_PORT%   New port: %NEW_PORT%
echo Old year: %OLD_YEAR%   New year: %NEW_YEAR%

rem --- Step 6: Build endpoint temp if port or year changed ---
if not defined NEW_PORT set "NEW_PORT=%OLD_PORT%"
if not defined NEW_YEAR set "NEW_YEAR=%OLD_YEAR%"
if /I "%NEW_PORT%"=="%OLD_PORT%" if /I "%NEW_YEAR%"=="%OLD_YEAR%" goto :NO_ENDPOINT_CHANGE

if not exist "%ENDPOINT_PATH%" (
    echo WARNING: client-endpoint.txt does not exist; it will not be updated.
    echo After applying the new port, update client connection settings manually.
    goto :NO_ENDPOINT_CHANGE
)
rem Build endpoint temp: copy and patch port= / god= lines
call :BUILD_ENDPOINT_TEMP
if errorlevel 1 goto :CLEANUP_TEMPS
set "ENDPOINT_MANAGED=1"

:NO_ENDPOINT_CHANGE

rem --- Step 7: Confirm ---
echo.
if defined ENDPOINT_MANAGED (
    echo Will update: Settings.json + client-endpoint.txt
) else (
    echo Will update: Settings.json only
)
choice /C YN /M "Apply this configuration? Stop service, replace, start, verify. [Y=apply, N=cancel]"
if errorlevel 2 goto :CANCELLED

rem --- Step 8: Stop service gracefully ---
echo.
echo Stopping %SERVICE_NAME%...
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :ALREADY_STOPPED

sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0
:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :SERVICE_STOPPED
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Still waiting for STOPPED... %WAIT_SECONDS%s / %STOP_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% goto :STOP_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP

:ALREADY_STOPPED
echo %SERVICE_NAME% is already STOPPED.

:SERVICE_STOPPED

rem --- Step 9: Apply config transaction (atomic replace + snapshot) ---
echo Applying configuration...
if defined ENDPOINT_MANAGED (
    "%HELPER%" settings-transaction-apply --data-dir "%DATA_DIR%" --settings-temp "%EDIT_TEMP%" --rollback-dir "%ROLLBACK_DIR%" --endpoint-temp "%ENDPOINT_TEMP%"
) else (
    "%HELPER%" settings-transaction-apply --data-dir "%DATA_DIR%" --settings-temp "%EDIT_TEMP%" --rollback-dir "%ROLLBACK_DIR%"
)
if errorlevel 1 (
    echo ERROR: settings-transaction-apply failed; files may be rolled back already.
    echo Review the output above and check %DATA_DIR%\Settings.json manually.
    call :START_SERVICE_BEST_EFFORT
    goto :CLEANUP_TEMPS
)
set "ROLLBACK_APPLIED=1"

rem --- Step 10: Update firewall rule if port changed ---
if /I "%NEW_PORT%"=="%OLD_PORT%" goto :SKIP_FIREWALL
call :UPDATE_FIREWALL
:SKIP_FIREWALL

rem --- Step 11: Start service ---
echo Starting %SERVICE_NAME%...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :START_FAILED_ROLLBACK

:WAIT_RUNNING
set /a WAIT_SECONDS=0
:WAIT_RUNNING_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :HEALTH_CHECK
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :START_FAILED_ROLLBACK
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 echo Waiting for RUNNING... %WAIT_SECONDS%s / %START_TIMEOUT_SECONDS%s
if %WAIT_SECONDS% GEQ %START_TIMEOUT_SECONDS% goto :START_FAILED_ROLLBACK
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING_LOOP

:HEALTH_CHECK
echo %SERVICE_NAME% reached RUNNING; checking PING/PONG on port %NEW_PORT%...
"%HELPER%" health --port %NEW_PORT% --timeout-ms 15000 >nul 2>&1
if errorlevel 1 goto :HEALTH_FAILED_ROLLBACK

rem --- Step 12: Commit transaction ---
echo PING/PONG confirmed. Committing transaction...
"%HELPER%" settings-transaction-commit --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%" >nul 2>&1

echo.
echo === Configuration applied successfully ===
echo Instance : %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo New port : %NEW_PORT%
echo Data dir : %DATA_DIR%
echo Logs     : %DATA_DIR%\logs
goto :CLEANUP_TEMPS_SUCCESS

rem ============================================================
:START_FAILED_ROLLBACK
echo ERROR: Service failed to reach RUNNING state after config apply.
goto :DO_ROLLBACK

:HEALTH_FAILED_ROLLBACK
echo ERROR: Service is RUNNING but PING/PONG failed on port %NEW_PORT%.
echo Initiating rollback...

:DO_ROLLBACK
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 (
    echo Stopping service before rollback...
    sc.exe stop "%SERVICE_NAME%" >nul 2>&1
    set /a RB_WAIT=0
    :ROLLBACK_STOP_LOOP
    sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
    if not errorlevel 1 goto :ROLLBACK_STOPPED
    set /a RB_WAIT+=1
    if %RB_WAIT% GEQ 120 goto :ROLLBACK_STOPPED
    ping.exe 127.0.0.1 -n 2 >nul
    goto :ROLLBACK_STOP_LOOP
)
:ROLLBACK_STOPPED

rem Rollback transaction
"%HELPER%" settings-transaction-rollback --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%" 2>&1

rem Restore firewall if modified
if defined FIREWALL_MODIFIED call :RESTORE_FIREWALL

rem Remove rollback dir
"%HELPER%" settings-transaction-commit --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%" >nul 2>&1

rem Restart service on old config
echo Restarting service on old configuration (port %OLD_PORT%)...
sc.exe start "%SERVICE_NAME%" >nul
set /a RB_WAIT=0
:ROLLBACK_START_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :ROLLBACK_HEALTH
set /a RB_WAIT+=1
if %RB_WAIT% GEQ %START_TIMEOUT_SECONDS% goto :ROLLBACK_START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :ROLLBACK_START_LOOP

:ROLLBACK_HEALTH
"%HELPER%" health --port %OLD_PORT% --timeout-ms 15000 >nul 2>&1
if not errorlevel 1 (
    echo Rollback complete. Service is RUNNING on old port %OLD_PORT%.
) else (
    echo WARNING: Service is RUNNING but PING/PONG on old port %OLD_PORT% failed.
    echo Check %DATA_DIR%\logs for details.
)
goto :CLEANUP_TEMPS

:ROLLBACK_START_FAILED
echo ERROR: Could not restart service after rollback.
echo Check %DATA_DIR%\logs and %DATA_DIR%\Settings.json manually.
goto :CLEANUP_TEMPS

rem ============================================================
:CANCELLED
echo Configuration cancelled. No changes were made.
goto :CLEANUP_TEMPS

:CLEANUP_TEMPS_SUCCESS
del /Q "%EDIT_TEMP%"     >nul 2>&1
del /Q "%ENDPOINT_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
pause
exit /b 0

:CLEANUP_TEMPS
del /Q "%EDIT_TEMP%"     >nul 2>&1
del /Q "%ENDPOINT_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
pause
exit /b 1

rem ============================================================
rem Subroutines
rem ============================================================

:BUILD_ENDPOINT_TEMP
rem Produce patched endpoint from live file: replace port= and god= lines.
rem All other lines are preserved.
if exist "%ENDPOINT_TEMP%" del /Q "%ENDPOINT_TEMP%"
setlocal EnableDelayedExpansion
for /f "usebackq delims=" %%L in ("%ENDPOINT_PATH%") do (
    set "LINE=%%L"
    set "KEY=!LINE:~0,5!"
    if /I "!KEY!"=="port=" (
        echo port=%NEW_PORT%>> "%ENDPOINT_TEMP%"
    ) else (
        set "KEY4=!LINE:~0,4!"
        if /I "!KEY4!"=="god=" (
            echo god=%NEW_YEAR%>> "%ENDPOINT_TEMP%"
        ) else (
            echo !LINE!>> "%ENDPOINT_TEMP%"
        )
    )
)
endlocal
if not exist "%ENDPOINT_TEMP%" (
    echo ERROR: Failed to build endpoint temp file.
    exit /b 1
)
exit /b 0

:UPDATE_FIREWALL
rem Update existing firewall rule "%FIREWALL_RULE%" to NEW_PORT.
rem Only touch it if it already exists.
netsh.exe advfirewall firewall show rule name="%FIREWALL_RULE%" >nul 2>&1
if errorlevel 1 (
    echo WARNING: Firewall rule "%FIREWALL_RULE%" not found; skipping update.
    exit /b 0
)
rem Record current firewall port for rollback
for /f "tokens=1,* delims=:" %%A in ('netsh.exe advfirewall firewall show rule name^="%FIREWALL_RULE%" verbose 2^>nul ^| findstr.exe /I /C:"LocalPort"') do set "OLD_FIREWALL_PORT=%%B"
if defined OLD_FIREWALL_PORT set "OLD_FIREWALL_PORT=%OLD_FIREWALL_PORT: =%"
echo Updating firewall rule "%FIREWALL_RULE%": %OLD_FIREWALL_PORT% -> %NEW_PORT%
netsh.exe advfirewall firewall set rule name="%FIREWALL_RULE%" new localport=%NEW_PORT% >nul 2>&1
if errorlevel 1 (
    echo WARNING: Could not update firewall rule. Verify manually.
) else (
    set "FIREWALL_MODIFIED=1"
)
exit /b 0

:RESTORE_FIREWALL
rem Restore the firewall rule to OLD_FIREWALL_PORT.
if not defined OLD_FIREWALL_PORT exit /b 0
netsh.exe advfirewall firewall show rule name="%FIREWALL_RULE%" >nul 2>&1
if errorlevel 1 exit /b 0
echo Restoring firewall rule "%FIREWALL_RULE%": %NEW_PORT% -> %OLD_FIREWALL_PORT%
netsh.exe advfirewall firewall set rule name="%FIREWALL_RULE%" new localport=%OLD_FIREWALL_PORT% >nul 2>&1
exit /b 0

:START_SERVICE_BEST_EFFORT
sc.exe start "%SERVICE_NAME%" >nul 2>&1
exit /b 0

:STOP_FAILED
echo ERROR: Service did not reach STOPPED state within %STOP_TIMEOUT_SECONDS% seconds.
echo No configuration changes were made.
sc.exe query "%SERVICE_NAME%"
goto :CLEANUP_TEMPS

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; first character must be alphanumeric.
pause
exit /b 1
