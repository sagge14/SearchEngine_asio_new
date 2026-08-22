@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem SVC-001: Safe installed-service configuration workflow.
rem Edits Settings.json of an installed SearchEngineService instance.
rem Reads the actual --data-dir from SCM (not %ProgramData%),
rem validates the edited copy, applies atomically via Stop->Start,
rem confirms PING/PONG, and rolls back automatically on any failure.
rem
rem Rollback contract:
rem   - File rollback is performed ONLY after SCM confirms STOPPED.
rem   - settings-transaction-commit is called ONLY after old config health verified.
rem   - Firewall restore failure leaves rollback-dir intact (no commit).
rem   - Snapshots are NEVER deleted after a failed rollback or failed health check.
rem
rem Requires Administrator elevation.

set "PACKAGE_ROOT=%~dp0"
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "STOP_TIMEOUT_SECONDS=1800"
set "START_TIMEOUT_SECONDS=1800"
set "UI_LANGUAGE=auto"

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

rem --- Instance selection ---
set "SERVICE_INSTANCE="
if not "%~1"=="" (
    set "SERVICE_INSTANCE=%~1"
    goto :SELECT_LANGUAGE_ONLY
)

rem No argument: use interactive picker
set "SELECTION_FILE=%TEMP%\SE-Configure-picker-%RANDOM%-%RANDOM%.txt"
"%HELPER%" choose-installed-instance --purpose configure --output "%SELECTION_FILE%"
set "PICKER_EXIT=%ERRORLEVEL%"
for /f "usebackq tokens=1,* delims==" %%A in ("%SELECTION_FILE%") do set "SELECTED_%%A=%%B"
del /Q "%SELECTION_FILE%" >nul 2>&1
if defined SELECTED_language set "UI_LANGUAGE=%SELECTED_language%"
if "%PICKER_EXIT%"=="3" goto :PICKER_NO_INSTALLED
if "%PICKER_EXIT%"=="2" goto :PICKER_CANCELLED
if not "%PICKER_EXIT%"=="0" goto :PICKER_HELPER_FAILED
if not defined SELECTED_instance goto :PICKER_HELPER_FAILED
if not defined SELECTED_language goto :PICKER_HELPER_FAILED
set "SERVICE_INSTANCE=%SELECTED_instance%"
goto :LANGUAGE_READY

:SELECT_LANGUAGE_ONLY
set "SELECTION_FILE=%TEMP%\SE-Configure-language-%RANDOM%-%RANDOM%.txt"
"%HELPER%" choose-language --output "%SELECTION_FILE%"
if errorlevel 1 goto :PICKER_HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%SELECTION_FILE%") do set "SELECTED_%%A=%%B"
del /Q "%SELECTION_FILE%" >nul 2>&1
if not defined SELECTED_language goto :PICKER_HELPER_FAILED
set "UI_LANGUAGE=%SELECTED_language%"
goto :LANGUAGE_READY

:PICKER_NO_INSTALLED
del /Q "%SELECTION_FILE%" >nul 2>&1
call :UI configure.no_services
call :PAUSE_UI
exit /b 1

:PICKER_CANCELLED
del /Q "%SELECTION_FILE%" >nul 2>&1
call :UI configure.cancelled
call :PAUSE_UI
exit /b 0

:PICKER_HELPER_FAILED
del /Q "%SELECTION_FILE%" >nul 2>&1
if /I "%UI_LANGUAGE%"=="auto" set "UI_LANGUAGE=en"
call :UI configure.helper_failed
call :PAUSE_UI
exit /b 1

:LANGUAGE_READY
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    call :UI configure.not_admin
    call :PAUSE_UI
    exit /b 1
)

:INSTANCE_RESOLVED
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
set "PORTABLE_FIREWALL_RULE=%SERVICE_NAME% TCP"

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
set "MATCHED_FIREWALL_RULE="
set "MATCHED_FIREWALL_TYPE="
set "OLD_PS_RULE_NAME="
set "NEW_PS_RULE_NAME="
set "FIREWALL_MODIFIED="
set "FIREWALL_MUTATION_STARTED="

rem Rollback tracking
set "ROLLBACK_APPLIED="
set "ENDPOINT_MANAGED="

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    call :UI configure.service_missing "%SERVICE_NAME%"
    call :PAUSE_UI
    exit /b 1
)

rem --- Step 1: Resolve actual data-dir via SCM ---
call :UI configure.resolving
chcp 65001 >nul
"%HELPER%" inspect-installed --instance "%SERVICE_INSTANCE%" > "%HELPER_OUTPUT%" 2>&1
if errorlevel 1 (
    type "%HELPER_OUTPUT%"
    del /Q "%HELPER_OUTPUT%" >nul 2>&1
    call :UI configure.resolve_failed "%SERVICE_NAME%"
    call :PAUSE_UI
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
    call :UI configure.inspect_missing
    call :PAUSE_UI
    exit /b 1
)
call :UI configure.instance_info "%SERVICE_INSTANCE%" "%SERVICE_NAME%" "%DATA_DIR%" "%SETTINGS_PATH%" "%ENDPOINT_PATH%"

rem --- Step 2: Inspect current settings (old port/year) ---
"%HELPER%" inspect --settings "%SETTINGS_PATH%" > "%HELPER_OUTPUT%" 2>&1
if errorlevel 1 (
    del /Q "%HELPER_OUTPUT%" >nul 2>&1
    call :UI configure.settings_invalid
    call :PAUSE_UI
    exit /b 1
)
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do (
    if /I "%%A"=="port" set "OLD_PORT=%%B"
    if /I "%%A"=="year" set "OLD_YEAR=%%B"
)
del /Q "%HELPER_OUTPUT%" >nul 2>&1
call :UI configure.current "%OLD_PORT%" "%OLD_YEAR%"

rem --- Step 3: Copy active Settings.json to temp for editing ---
copy /Y "%SETTINGS_PATH%" "%EDIT_TEMP%" >nul
if errorlevel 1 (
    call :UI configure.copy_failed
    goto :CLEANUP_TEMPS_ERR
)

rem --- Step 3b: Pretty-format temp copy once for Notepad readability ---
"%HELPER%" format-json --settings "%EDIT_TEMP%" --line-ending crlf
if errorlevel 1 (
    call :UI configure.format_failed
    goto :CLEANUP_TEMPS_ERR
)

rem --- Step 4: Open temp file for editing; loop until valid ---
:EDIT_LOOP
notepad.exe "%EDIT_TEMP%"

"%HELPER%" validate --settings "%EDIT_TEMP%" > "%HELPER_OUTPUT%" 2>&1
if not errorlevel 1 goto :EDIT_VALID

call :UI configure.validation_header
type "%HELPER_OUTPUT%"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
call :UI configure.validation_footer
call :CHOICE 12
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
call :UI configure.old_new "%OLD_PORT%" "%NEW_PORT%" "%OLD_YEAR%" "%NEW_YEAR%"

rem --- Step 6: Build endpoint temp if port or year changed ---
if not defined NEW_PORT set "NEW_PORT=%OLD_PORT%"
if not defined NEW_YEAR set "NEW_YEAR=%OLD_YEAR%"
if /I "%NEW_PORT%"=="%OLD_PORT%" if /I "%NEW_YEAR%"=="%OLD_YEAR%" goto :NO_ENDPOINT_CHANGE

if not exist "%ENDPOINT_PATH%" (
    call :UI configure.endpoint_missing
    goto :NO_ENDPOINT_CHANGE
)
call :BUILD_ENDPOINT_TEMP
if errorlevel 1 goto :CLEANUP_TEMPS_ERR
set "ENDPOINT_MANAGED=1"

:NO_ENDPOINT_CHANGE

rem --- Step 7: Confirm ---
if defined ENDPOINT_MANAGED (
    call :UI configure.confirm_both
) else (
    call :UI configure.confirm_settings
)
call :UI configure.apply_menu
call :CHOICE 12
if errorlevel 2 goto :CANCELLED

rem --- Step 8: Stop service gracefully ---
call :UI configure.stopping "%SERVICE_NAME%"
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :SERVICE_STOPPED

sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0

:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :SERVICE_STOPPED
set /a WAIT_SECONDS+=1
set /a PROGRESS_MOD=WAIT_SECONDS %% 30
if %PROGRESS_MOD%==0 call :UI configure.wait_stopped "%WAIT_SECONDS%" "%STOP_TIMEOUT_SECONDS%"
if %WAIT_SECONDS% GEQ %STOP_TIMEOUT_SECONDS% goto :STOP_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP

:SERVICE_STOPPED
call :UI configure.stopped "%SERVICE_NAME%"

rem --- Step 9: Apply config transaction (atomic replace + snapshot) ---
call :UI configure.applying
if defined ENDPOINT_MANAGED (
    "%HELPER%" settings-transaction-apply --data-dir "%DATA_DIR%" --settings-temp "%EDIT_TEMP%" --rollback-dir "%ROLLBACK_DIR%" --endpoint-temp "%ENDPOINT_TEMP%"
) else (
    "%HELPER%" settings-transaction-apply --data-dir "%DATA_DIR%" --settings-temp "%EDIT_TEMP%" --rollback-dir "%ROLLBACK_DIR%"
)
if errorlevel 1 (
    call :UI configure.apply_failed "%DATA_DIR%"
    call :START_SERVICE_BEST_EFFORT
    goto :CLEANUP_TEMPS_ERR
)
set "ROLLBACK_APPLIED=1"

rem --- Step 10: Update firewall rule if port changed ---
if /I "%NEW_PORT%"=="%OLD_PORT%" goto :SKIP_FIREWALL
call :UPDATE_FIREWALL
if errorlevel 1 goto :START_FAILED_ROLLBACK
:SKIP_FIREWALL

rem --- Step 11: Start service ---
call :UI configure.starting "%SERVICE_NAME%"
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
if %PROGRESS_MOD%==0 call :UI configure.wait_running "%WAIT_SECONDS%" "%START_TIMEOUT_SECONDS%"
if %WAIT_SECONDS% GEQ %START_TIMEOUT_SECONDS% goto :START_FAILED_ROLLBACK
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING_LOOP

:HEALTH_CHECK
call :UI configure.health "%SERVICE_NAME%" "%NEW_PORT%"
"%HELPER%" health --port %NEW_PORT% --timeout-ms 15000 >nul 2>&1
if errorlevel 1 goto :HEALTH_FAILED_ROLLBACK

rem --- Step 12: Commit transaction ---
call :UI configure.committing
"%HELPER%" settings-transaction-commit --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%"
if errorlevel 1 (
    call :UI configure.commit_warning "%ROLLBACK_DIR%"
)

call :UI configure.success "%SERVICE_INSTANCE%" "%SERVICE_NAME%" "%NEW_PORT%" "%DATA_DIR%"
goto :CLEANUP_TEMPS_OK

rem ============================================================
:START_FAILED_ROLLBACK
call :UI configure.start_failed
goto :DO_ROLLBACK

:HEALTH_FAILED_ROLLBACK
call :UI configure.health_failed "%NEW_PORT%"

rem ============================================================
rem ROLLBACK SEQUENCE
rem
rem Invariant: NO byte of Settings.json is restored unless SCM
rem explicitly shows STOPPED on the most recent query.
rem ============================================================
:DO_ROLLBACK

rem Determine current SCM state
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :ROLLBACK_DO_FILES

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]3[ ]*STOP_PENDING" >nul
if not errorlevel 1 goto :ROLLBACK_WAIT_STOPPED

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :ROLLBACK_ISSUE_STOP

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]2[ ]*START_PENDING" >nul
if not errorlevel 1 goto :ROLLBACK_ISSUE_STOP

rem Unknown/unsupported transitional state - do not touch files
call :UI configure.unexpected_state
sc.exe query "%SERVICE_NAME%"
goto :ROLLBACK_CANNOT_STOP

:ROLLBACK_ISSUE_STOP
call :UI configure.stopping_rollback "%SERVICE_NAME%"
sc.exe stop "%SERVICE_NAME%" >nul 2>&1

:ROLLBACK_WAIT_STOPPED
set /a RB_WAIT=0

:ROLLBACK_WAIT_LOOP
rem Re-check state on each iteration (service may cycle through states)
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :ROLLBACK_DO_FILES

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 (
    rem Transitioned back to RUNNING (e.g. start completed during stop attempt)
    sc.exe stop "%SERVICE_NAME%" >nul 2>&1
)

sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]2[ ]*START_PENDING" >nul
if not errorlevel 1 (
    rem Still starting; stop best-effort and keep waiting
    sc.exe stop "%SERVICE_NAME%" >nul 2>&1
)

set /a RB_WAIT+=1
set /a PROGRESS_MOD=RB_WAIT %% 30
if %PROGRESS_MOD%==0 call :UI configure.wait_rollback "%RB_WAIT%" "%STOP_TIMEOUT_SECONDS%"
if %RB_WAIT% GEQ %STOP_TIMEOUT_SECONDS% goto :ROLLBACK_CANNOT_STOP
ping.exe 127.0.0.1 -n 2 >nul
goto :ROLLBACK_WAIT_LOOP

:ROLLBACK_CANNOT_STOP
call :UI configure.rollback_cannot_stop "%STOP_TIMEOUT_SECONDS%" "%SERVICE_NAME%" "%DATA_DIR%" "%ROLLBACK_DIR%" "%OLD_PORT%" "%NEW_PORT%"
sc.exe query "%SERVICE_NAME%"
goto :CLEANUP_TEMPS_ERR_KEEP_ROLLBACK

:ROLLBACK_DO_FILES
call :UI configure.restoring_files
"%HELPER%" settings-transaction-rollback --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%"
if errorlevel 1 (
    call :UI configure.rollback_files_failed "%ROLLBACK_DIR%"
    goto :CLEANUP_TEMPS_ERR_KEEP_ROLLBACK
)
call :UI configure.files_restored

rem --- Restore firewall if it was modified ---
if not defined FIREWALL_MODIFIED if not defined FIREWALL_MUTATION_STARTED goto :ROLLBACK_FIREWALL_DONE
call :RESTORE_FIREWALL_CHECKED
if errorlevel 1 (
    call :UI configure.firewall_restore_failed "%NEW_PORT%" "%ROLLBACK_DIR%"
    rem Old service can still be started (files are restored), but rollback incomplete
    call :START_SERVICE_BEST_EFFORT
    goto :CLEANUP_TEMPS_ERR_KEEP_ROLLBACK
)
:ROLLBACK_FIREWALL_DONE

rem --- Restart service on old configuration ---
call :UI configure.restart_old "%OLD_PORT%"
sc.exe start "%SERVICE_NAME%" >nul
set /a RB_WAIT=0
:ROLLBACK_START_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 goto :ROLLBACK_HEALTH
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :ROLLBACK_START_FAILED
set /a RB_WAIT+=1
if %RB_WAIT% GEQ %START_TIMEOUT_SECONDS% goto :ROLLBACK_START_FAILED
ping.exe 127.0.0.1 -n 2 >nul
goto :ROLLBACK_START_LOOP

:ROLLBACK_START_FAILED
call :UI configure.old_start_failed "%ROLLBACK_DIR%" "%DATA_DIR%"
goto :CLEANUP_TEMPS_ERR_KEEP_ROLLBACK

:ROLLBACK_HEALTH
"%HELPER%" health --port %OLD_PORT% --timeout-ms 15000 >nul 2>&1
if errorlevel 1 (
    call :UI configure.old_health_failed "%OLD_PORT%" "%ROLLBACK_DIR%" "%DATA_DIR%"
    goto :CLEANUP_TEMPS_ERR_KEEP_ROLLBACK
)
call :UI configure.rollback_success "%OLD_PORT%"

rem Only commit after old config health is confirmed
"%HELPER%" settings-transaction-commit --data-dir "%DATA_DIR%" --rollback-dir "%ROLLBACK_DIR%"
if errorlevel 1 (
    call :UI configure.rollback_commit_warning "%ROLLBACK_DIR%"
)
goto :CLEANUP_TEMPS_ERR

rem ============================================================
:CANCELLED
call :UI configure.cancelled
goto :CLEANUP_TEMPS_OK

:STOP_FAILED
call :UI configure.stop_failed "%STOP_TIMEOUT_SECONDS%"
sc.exe query "%SERVICE_NAME%"
goto :CLEANUP_TEMPS_ERR

:INVALID_INSTANCE
call :UI common.invalid_instance "%SERVICE_INSTANCE%"
call :PAUSE_UI
exit /b 1

rem ============================================================
rem Cleanup labels
rem ============================================================
:CLEANUP_TEMPS_OK
del /Q "%EDIT_TEMP%"     >nul 2>&1
del /Q "%ENDPOINT_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
call :PAUSE_UI
exit /b 0

:CLEANUP_TEMPS_ERR
del /Q "%EDIT_TEMP%"     >nul 2>&1
del /Q "%ENDPOINT_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
call :PAUSE_UI
exit /b 1

:CLEANUP_TEMPS_ERR_KEEP_ROLLBACK
rem Rollback-dir intentionally preserved for manual recovery
del /Q "%EDIT_TEMP%"     >nul 2>&1
del /Q "%ENDPOINT_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
call :PAUSE_UI
exit /b 1

rem ============================================================
rem Subroutines
rem ============================================================

:BUILD_ENDPOINT_TEMP
rem Produce patched endpoint from live file: replace port= and god= lines.
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
    call :UI configure.endpoint_temp_failed
    exit /b 1
)
exit /b 0

:UPDATE_FIREWALL
rem Find the installer-owned firewall rule and update its port.
rem Portable rule: SearchEngineService[-instance] TCP  (stable name, only localport changes)
rem PowerShell rule: <display name> (<old_port>/TCP)   (name includes port; must be renamed)
rem If a known installer rule is found but update fails, return errorlevel 1.
set "MATCHED_FIREWALL_RULE="
set "MATCHED_FIREWALL_TYPE="
set "OLD_PS_RULE_NAME="
set "NEW_PS_RULE_NAME="

netsh.exe advfirewall firewall show rule name="%PORTABLE_FIREWALL_RULE%" >nul 2>&1
if not errorlevel 1 (
    set "MATCHED_FIREWALL_RULE=%PORTABLE_FIREWALL_RULE%"
    set "MATCHED_FIREWALL_TYPE=portable"
    goto :DO_FIREWALL_UPDATE
)

set "PS_FIREWALL_OLD=%DISPLAY_NAME% (%OLD_PORT%/TCP)"
netsh.exe advfirewall firewall show rule name="%PS_FIREWALL_OLD%" >nul 2>&1
if not errorlevel 1 (
    set "MATCHED_FIREWALL_RULE=%PS_FIREWALL_OLD%"
    set "MATCHED_FIREWALL_TYPE=ps"
    set "OLD_PS_RULE_NAME=%PS_FIREWALL_OLD%"
    set "NEW_PS_RULE_NAME=%DISPLAY_NAME% (%NEW_PORT%/TCP)"
    goto :DO_FIREWALL_UPDATE
)

call :UI configure.firewall_none
exit /b 0

:DO_FIREWALL_UPDATE
if /I "%MATCHED_FIREWALL_TYPE%"=="portable" (
    call :UI configure.firewall_update "%MATCHED_FIREWALL_RULE%" "%OLD_PORT%" "%NEW_PORT%"
    netsh.exe advfirewall firewall set rule name="%MATCHED_FIREWALL_RULE%" new localport=%NEW_PORT% >nul 2>&1
    if errorlevel 1 (
        call :UI configure.firewall_update_failed "%MATCHED_FIREWALL_RULE%"
        exit /b 1
    )
    set "FIREWALL_MODIFIED=1"
    exit /b 0
)

rem PowerShell-style rule: delete old name, create new name with new port
call :UI configure.firewall_rename "%OLD_PS_RULE_NAME%" "%NEW_PS_RULE_NAME%"
rem Export the existing rule's remoteport/protocol/direction before deletion
rem (contract baseline preserved: inbound TCP allow, new TCP port, exact SearchEngine.exe program binding, enabled)
if not defined PROGRAM_PATH (
    call :UI configure.firewall_program_empty
    exit /b 1
)
netsh.exe advfirewall firewall delete rule name="%OLD_PS_RULE_NAME%" >nul 2>&1
if errorlevel 1 (
    call :UI configure.firewall_delete_old_failed "%OLD_PS_RULE_NAME%"
    exit /b 1
)
set "FIREWALL_MUTATION_STARTED=1"
netsh.exe advfirewall firewall add rule name="%NEW_PS_RULE_NAME%" dir=in action=allow protocol=TCP localport=%NEW_PORT% program="%PROGRAM_PATH%" enable=yes >nul 2>&1
if errorlevel 1 (
    call :UI configure.firewall_create_new_failed "%NEW_PS_RULE_NAME%"
    exit /b 1
)
set "FIREWALL_MODIFIED=1"
exit /b 0

:RESTORE_FIREWALL_CHECKED
rem Restore firewall to old state. Returns errorlevel 1 if known rule restore fails.
if /I "%MATCHED_FIREWALL_TYPE%"=="portable" (
    call :UI configure.firewall_restore "%MATCHED_FIREWALL_RULE%" "%NEW_PORT%" "%OLD_PORT%"
    netsh.exe advfirewall firewall set rule name="%MATCHED_FIREWALL_RULE%" new localport=%OLD_PORT% >nul 2>&1
    if errorlevel 1 (
        call :UI configure.firewall_restore_failed_rule "%MATCHED_FIREWALL_RULE%"
        exit /b 1
    )
    exit /b 0
)

if /I "%MATCHED_FIREWALL_TYPE%"=="ps" (
    call :UI configure.firewall_restore_rename "%NEW_PS_RULE_NAME%" "%OLD_PS_RULE_NAME%"
    if not defined PROGRAM_PATH (
        call :UI configure.firewall_program_restore_empty
        exit /b 1
    )
    rem NEW-rule: delete only if it exists; always check delete errorlevel.
    netsh.exe advfirewall firewall show rule name="%NEW_PS_RULE_NAME%" >nul 2>&1
    if not errorlevel 1 (
        netsh.exe advfirewall firewall delete rule name="%NEW_PS_RULE_NAME%" >nul 2>&1
        if errorlevel 1 (
            call :UI configure.firewall_delete_new_failed "%NEW_PS_RULE_NAME%"
            exit /b 1
        )
    )

    rem OLD-rule: create/restore with checked errorlevel.
    netsh.exe advfirewall firewall add rule name="%OLD_PS_RULE_NAME%" dir=in action=allow protocol=TCP localport=%OLD_PORT% program="%PROGRAM_PATH%" enable=yes >nul 2>&1
    if errorlevel 1 (
        call :UI configure.firewall_restore_old_failed "%OLD_PS_RULE_NAME%"
        exit /b 1
    )

    rem Contract check: OLD must exist; NEW must not exist after restore.
    netsh.exe advfirewall firewall show rule name="%OLD_PS_RULE_NAME%" >nul 2>&1
    if errorlevel 1 (
        call :UI configure.firewall_verify_old_missing "%OLD_PS_RULE_NAME%"
        exit /b 1
    )
    netsh.exe advfirewall firewall show rule name="%NEW_PS_RULE_NAME%" >nul 2>&1
    if not errorlevel 1 (
        call :UI configure.firewall_verify_new_exists "%NEW_PS_RULE_NAME%"
        exit /b 1
    )
    exit /b 0
)

exit /b 0

:START_SERVICE_BEST_EFFORT
sc.exe start "%SERVICE_NAME%" >nul 2>&1
exit /b 0

:CHOICE
call :UI common.select
choice.exe /C %~1 /N /M ""
exit /b %ERRORLEVEL%

:PAUSE_UI
if /I "%UI_LANGUAGE%"=="auto" (
    pause
) else (
    call :UI common.press_any_key
    pause >nul
)
exit /b 0

:UI
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id "%~1" --arg1 "%~2" --arg2 "%~3" --arg3 "%~4" --arg4 "%~5" --arg5 "%~6" --arg6 "%~7" --arg7 "%~8"
exit /b 0
