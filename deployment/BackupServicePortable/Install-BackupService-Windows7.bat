@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "SERVICE_INSTANCE=default"
if exist "%PACKAGE_ROOT%ServiceInstance.cmd" call "%PACKAGE_ROOT%ServiceInstance.cmd"
set "VALIDATE_ONLY=0"
set "SKIP_VC_REDIST=0"
set "INSTANCE_FROM_ARGS=0"
set "BAD_ARG="

:PARSE_ARGS
if "%~1"=="" goto :ARGS_DONE
if /I "%~1"=="/validate" (
    set "VALIDATE_ONLY=1"
    shift
    goto :PARSE_ARGS
)
if /I "%~1"=="/SkipVcRedist" (
    set "SKIP_VC_REDIST=1"
    shift
    goto :PARSE_ARGS
)
set "ARG=%~1"
if "%ARG:~0,1%"=="/" (
    set "BAD_ARG=%~1"
    goto :UNKNOWN_ARGUMENT
)
if "%INSTANCE_FROM_ARGS%"=="1" (
    set "BAD_ARG=%~1"
    goto :UNKNOWN_ARGUMENT
)
set "SERVICE_INSTANCE=%~1"
set "INSTANCE_FROM_ARGS=1"
shift
goto :PARSE_ARGS

:ARGS_DONE
set "SERVICE_NAME=SearchEngineBackupService"
set "DISPLAY_NAME=SearchEngine Backup Service"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "DATA_DIR=%ProgramData%\SearchEngineBackupService"
set "BINARY=%PACKAGE_ROOT%app\BackupService.exe"
set "CONFIG_TEMPLATE=%PACKAGE_ROOT%data\Backup.json"
set "VC_REDIST=%PACKAGE_ROOT%prerequisites\{{VC_REDIST_FILE}}"
set "ROLLBACK_READY=0"
set "REINSTALL=0"
set "BACKUP_MODE=none"

if /I "%TARGET_ARCH%"=="x86" goto :SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramW6432%"
if "%PROGRAM_ROOT%"=="" set "PROGRAM_ROOT=%ProgramFiles%"
goto :ROOT_READY

:SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramFiles%"
if not "%ProgramFiles(x86)%"=="" set "PROGRAM_ROOT=%ProgramFiles(x86)%"

:ROOT_READY
set "INSTALL_ROOT=%PROGRAM_ROOT%\SearchEngineBackupService"
set "INSTALLED_BIN=%INSTALL_ROOT%\bin"

echo SearchEngineBackupService portable installer ^(%TARGET_ARCH%^)
echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo.

if not exist "%BINARY%" goto :PACKAGE_MISSING
if not exist "%CONFIG_TEMPLATE%" goto :PACKAGE_MISSING
if not exist "%VC_REDIST%" goto :PACKAGE_MISSING
if not exist "%PACKAGE_ROOT%ServiceInstance.cmd" goto :PACKAGE_MISSING
if not exist "%PACKAGE_ROOT%Verify-Package.bat" goto :PACKAGE_MISSING

call "%PACKAGE_ROOT%Verify-Package.bat" /quiet
if errorlevel 1 goto :PACKAGE_DAMAGED

"%BINARY%" --validate-config --config "%CONFIG_TEMPLATE%" --data-dir "%PACKAGE_ROOT%data"
if errorlevel 1 goto :CONFIG_INVALID

if "%VALIDATE_ONLY%"=="1" goto :VALIDATED

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 goto :NOT_ADMIN

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 goto :CLEAN_DESTINATION_CHECK
set "REINSTALL=1"
echo An installed SearchEngineBackupService was found.
echo   1 - Reinstall or update it ^(recommended^)
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED
call :CHOOSE_BACKUP
if errorlevel 1 goto :CANCELLED
goto :INSTALL_STEPS

:CLEAN_DESTINATION_CHECK
set "LEFTOVER_DIRECTORIES=0"
call :CHECK_EMPTY_DIRECTORY "%INSTALL_ROOT%"
if errorlevel 1 set "LEFTOVER_DIRECTORIES=1"
call :CHECK_EMPTY_DIRECTORY "%DATA_DIR%"
if errorlevel 1 set "LEFTOVER_DIRECTORIES=1"
if "%LEFTOVER_DIRECTORIES%"=="0" goto :INSTALL_STEPS

echo.
echo Files remain from an earlier incomplete uninstall, but the Windows
echo service %SERVICE_NAME% is not registered.
if exist "%INSTALL_ROOT%\" echo   Application: %INSTALL_ROOT%
if exist "%DATA_DIR%\" echo   Data:        %DATA_DIR%
echo.
echo Removing these folders permanently deletes their Backup.json and logs.
echo Snapshot and mirror-history folders from backup_dir are not touched.
echo   1 - Delete the leftover folders and continue installation
echo   2 - Cancel ^(recommended if the files must be preserved^)
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED
call :DELETE_DIRECTORY_RETRY "%INSTALL_ROOT%"
if errorlevel 1 goto :INSTALL_LEFTOVER_DELETE_FAILED
call :DELETE_DIRECTORY_RETRY "%DATA_DIR%"
if errorlevel 1 goto :DATA_LEFTOVER_DELETE_FAILED

:INSTALL_STEPS
echo.
echo Configuration file: %CONFIG_TEMPLATE%
echo Edit data\Backup.json in the portable folder before installation if paths
echo must match this computer. The installer copies it into ProgramData as-is.
echo.
echo   1 - Continue installation
echo   2 - Cancel and edit Backup.json
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED

echo [1/6] Ensuring Microsoft Visual C++ Runtime...
if "%SKIP_VC_REDIST%"=="1" goto :REDIST_SKIP_FLAG
set "CRT_PROBE_OK=0"
call :PROBE_VC_RUNTIME
if not errorlevel 1 set "CRT_PROBE_OK=1"
if "%CRT_PROBE_OK%"=="1" goto :REDIST_ASK_WHEN_PRESENT
echo Visual C++ Runtime files were not detected for architecture %TARGET_ARCH%.
echo   1 - Install or update the packaged redistributable ^(recommended^)
echo   2 - Skip redistributable setup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :REDIST_SKIP_CHOICE
goto :REDIST_INSTALL

:REDIST_ASK_WHEN_PRESENT
echo Visual C++ Runtime files were found on this computer.
echo   1 - Skip redistributable setup ^(recommended^)
echo   2 - Install or update the packaged redistributable anyway
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :REDIST_INSTALL
goto :REDIST_SKIP_CHOICE

:REDIST_INSTALL
echo Installing Microsoft Visual C++ Runtime...
start "" /wait "%VC_REDIST%" /install /quiet /norestart
set "REDIST_EXIT=%ERRORLEVEL%"
if "%REDIST_EXIT%"=="0" goto :REDIST_OK
if "%REDIST_EXIT%"=="1638" goto :REDIST_OK
if "%REDIST_EXIT%"=="3010" goto :REDIST_RESTART
echo WARNING: Visual C++ Runtime setup failed with exit code %REDIST_EXIT%.
echo BackupService.exe already ran, so the runtime is likely already present.
echo   1 - Continue installation
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :FAILED
goto :REDIST_OK

:REDIST_SKIP_FLAG
echo Skipping Visual C++ Runtime setup because /SkipVcRedist was specified.
goto :REDIST_OK

:REDIST_SKIP_CHOICE
echo Skipping Visual C++ Runtime redistributable setup by user choice.
goto :REDIST_OK

:REDIST_RESTART
echo WARNING: Windows must be restarted after the installation.

:REDIST_OK
if "%REINSTALL%"=="0" goto :COPY_FILES
echo [2/6] Stopping the installed service...
call :STOP_SERVICE
if errorlevel 1 goto :FAILED

if "%BACKUP_MODE%"=="none" goto :PREPARE_ROLLBACK
echo [3/6] Exporting the previous settings and logs...
call :EXPORT_SETTINGS_LOGS
if errorlevel 1 goto :RESTART_OLD_SERVICE_AND_FAIL

:PREPARE_ROLLBACK
set "ROLLBACK_INSTALL=%INSTALL_ROOT%.rollback-%RANDOM%-%RANDOM%"
set "ROLLBACK_DATA=%DATA_DIR%.rollback-%RANDOM%-%RANDOM%"
if exist "%ROLLBACK_INSTALL%" goto :ROLLBACK_PREPARE_FAILED
if exist "%ROLLBACK_DATA%" goto :ROLLBACK_PREPARE_FAILED
if exist "%INSTALL_ROOT%" move "%INSTALL_ROOT%" "%ROLLBACK_INSTALL%" >nul
if exist "%INSTALL_ROOT%" goto :ROLLBACK_PREPARE_FAILED
if exist "%DATA_DIR%" move "%DATA_DIR%" "%ROLLBACK_DATA%" >nul
if exist "%DATA_DIR%" goto :ROLLBACK_PREPARE_FAILED
set "ROLLBACK_READY=1"
goto :COPY_FILES

:COPY_FILES
echo [4/6] Copying application and data files...
md "%INSTALLED_BIN%" >nul 2>&1
md "%DATA_DIR%" >nul 2>&1
md "%DATA_DIR%\logs" >nul 2>&1
if not exist "%INSTALLED_BIN%\" goto :COPY_FAILED
if not exist "%DATA_DIR%\" goto :COPY_FAILED
xcopy.exe "%PACKAGE_ROOT%app\*" "%INSTALLED_BIN%\" /E /I /H /R /Y >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%README.txt" "%INSTALL_ROOT%\README.txt" >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%INSTALLATION_GUIDE_RU.txt" "%INSTALL_ROOT%\INSTALLATION_GUIDE_RU.txt" >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%ServiceInstance.cmd" "%INSTALL_ROOT%\ServiceInstance.cmd" >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%CONFIG_TEMPLATE%" "%DATA_DIR%\Backup.json" >nul
if errorlevel 1 goto :COPY_FAILED

"%INSTALLED_BIN%\BackupService.exe" --validate-config --config "%DATA_DIR%\Backup.json" --data-dir "%DATA_DIR%"
if errorlevel 1 goto :INSTALLED_CONFIG_INVALID

echo [5/6] Registering and configuring the Windows service...
if "%REINSTALL%"=="1" goto :CONFIG_EXISTING_SERVICE
sc.exe create "%SERVICE_NAME%" binPath= "\"%INSTALLED_BIN%\BackupService.exe\" --service --service-name \"%SERVICE_NAME%\" --config \"%DATA_DIR%\Backup.json\" --data-dir \"%DATA_DIR%\"" start= delayed-auto DisplayName= "%DISPLAY_NAME%" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
goto :CONFIGURE_SERVICE_COMMON

:CONFIG_EXISTING_SERVICE
sc.exe config "%SERVICE_NAME%" binPath= "\"%INSTALLED_BIN%\BackupService.exe\" --service --service-name \"%SERVICE_NAME%\" --config \"%DATA_DIR%\Backup.json\" --data-dir \"%DATA_DIR%\"" start= delayed-auto DisplayName= "%DISPLAY_NAME%" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED

:CONFIGURE_SERVICE_COMMON
sc.exe description "%SERVICE_NAME%" "Scheduled snapshot and SQLite backup service" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
sc.exe failure "%SERVICE_NAME%" reset= 86400 actions= restart/60000/restart/60000/restart/300000 >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
sc.exe failureflag "%SERVICE_NAME%" 1 >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" /v PreshutdownTimeout /t REG_DWORD /d 1800000 /f >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED

echo [6/6] Starting the service...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :SERVICE_START_FAILED
call :WAIT_FOR_RUNNING
if errorlevel 1 goto :SERVICE_START_FAILED

if "%ROLLBACK_READY%"=="0" goto :INSTALLED
rmdir /S /Q "%ROLLBACK_INSTALL%" >nul 2>&1
if exist "%ROLLBACK_INSTALL%" echo WARNING: old application directory could not be removed: %ROLLBACK_INSTALL%
rmdir /S /Q "%ROLLBACK_DATA%" >nul 2>&1
if exist "%ROLLBACK_DATA%" echo WARNING: old data directory could not be removed: %ROLLBACK_DATA%
set "ROLLBACK_READY=0"

:INSTALLED
echo.
echo Installation completed successfully.
echo Service:     %SERVICE_NAME% ^(RUNNING^)
echo Application: %INSTALLED_BIN%
echo Data:        %DATA_DIR%
echo Config:      %DATA_DIR%\Backup.json
echo Logs:        %DATA_DIR%\logs
echo.
echo Snapshot directories live under backup_dir from Backup.json and are separate
echo from ProgramData.
echo.
pause
exit /b 0

:VALIDATED
echo Package and Backup.json validation completed successfully.
exit /b 0

:CHOOSE_BACKUP
echo.
echo Backup before replacing the installed files:
echo   1 - Settings and logs ^(recommended^)
echo   2 - Do not create a backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CONFIRM_NO_BACKUP
set "BACKUP_MODE=settings-logs"
:READ_BACKUP_DESTINATION
set "BACKUP_DESTINATION="
set /p "BACKUP_DESTINATION=Destination disk or folder, for example E:\Backups: "
if "%BACKUP_DESTINATION%"=="" goto :READ_BACKUP_DESTINATION
exit /b 0
:CONFIRM_NO_BACKUP
echo No backup means that old settings and logs will be deleted
echo only after the new service reaches RUNNING.
echo   1 - Cancel ^(recommended^)
echo   2 - Continue without backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 set "BACKUP_MODE=none"
if errorlevel 2 exit /b 0
exit /b 1

:EXPORT_SETTINGS_LOGS
set "STAMP="
for /f "tokens=1-3 delims=/. " %%A in ("%DATE%") do set "STAMP=%%C%%B%%A"
for /f "tokens=1-2 delims=:." %%A in ("%TIME%") do set "STAMP=%STAMP%-%%A%%B"
set "STAMP=%STAMP: =0%"
set "EXPORT_ROOT=%BACKUP_DESTINATION%\SearchEngineBackupService-reinstall-%STAMP%"
md "%EXPORT_ROOT%" >nul 2>&1
if not exist "%EXPORT_ROOT%\" exit /b 1
if exist "%DATA_DIR%\Backup.json" copy /Y "%DATA_DIR%\Backup.json" "%EXPORT_ROOT%\Backup.json" >nul
if exist "%DATA_DIR%\logs\" xcopy.exe "%DATA_DIR%\logs\*" "%EXPORT_ROOT%\logs\" /E /I /H /R /Y >nul
echo Backup exported to: %EXPORT_ROOT%
exit /b 0

:PROBE_VC_RUNTIME
REM Prefer loading BackupService.exe: same /MD CRT as the installed binary.
REM File probes alone are unreliable under 32-bit cmd (System32 -> SysWOW64).
"%BINARY%" --validate-config --config "%CONFIG_TEMPLATE%" --data-dir "%PACKAGE_ROOT%data" >nul 2>&1
if not errorlevel 1 exit /b 0

if /I "%TARGET_ARCH%"=="x64" goto :PROBE_VC_X64
set "CRT_DIR=%SystemRoot%\System32"
if not "%ProgramFiles(x86)%"=="" set "CRT_DIR=%SystemRoot%\SysWOW64"
goto :PROBE_VC_CHECK

:PROBE_VC_X64
set "CRT_DIR=%SystemRoot%\System32"
if exist "%SystemRoot%\Sysnative\" set "CRT_DIR=%SystemRoot%\Sysnative"
goto :PROBE_VC_CHECK

:PROBE_VC_CHECK
if not exist "%CRT_DIR%\vcruntime140.dll" exit /b 1
if not exist "%CRT_DIR%\msvcp140.dll" exit /b 1
if not exist "%CRT_DIR%\vcruntime140_1.dll" exit /b 1
exit /b 0

:STOP_SERVICE
set "STOPPED_SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "STOPPED_SERVICE_PID=%%P"
set "STOPPED_SERVICE_PID=%STOPPED_SERVICE_PID: =%"
echo(%STOPPED_SERVICE_PID%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 set "STOPPED_SERVICE_PID="
sc.exe stop "%SERVICE_NAME%" >nul 2>&1
set /a WAIT_SECONDS=0
:WAIT_STOPPED_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 goto :OFFER_FORCE_STOP
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_LOOP
:OFFER_FORCE_STOP
echo The service did not stop within 120 seconds.
echo   1 - Force-terminate its process and continue
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
set "SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /R /C:":[ ]*[1-9][0-9]*[ ]*$"') do set "SERVICE_PID=%%P"
set "SERVICE_PID=%SERVICE_PID: =%"
echo(%SERVICE_PID%| findstr.exe /R /X "[1-9][0-9]*" >nul
if errorlevel 1 exit /b 1
taskkill.exe /PID %SERVICE_PID% /T /F >nul 2>&1
if errorlevel 1 exit /b 1
ping.exe 127.0.0.1 -n 3 >nul
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if errorlevel 1 exit /b 1
exit /b 0

:WAIT_STOPPED_PROCESS_EXIT
if not defined STOPPED_SERVICE_PID exit /b 0
set /a PROCESS_WAIT_SECONDS=0
:WAIT_STOPPED_PROCESS_LOOP
tasklist.exe /FI "PID eq %STOPPED_SERVICE_PID%" /NH 2>nul | findstr.exe /R /C:"[ ]%STOPPED_SERVICE_PID%[ ]" >nul
if errorlevel 1 exit /b 0
set /a PROCESS_WAIT_SECONDS+=1
if %PROCESS_WAIT_SECONDS% GEQ 30 goto :OFFER_FORCE_STOPPED_PROCESS
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED_PROCESS_LOOP

:OFFER_FORCE_STOPPED_PROCESS
echo The service is STOPPED, but process PID %STOPPED_SERVICE_PID% still holds files.
echo   1 - Force-terminate this service process and continue
echo   2 - Cancel without deleting files
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
taskkill.exe /PID %STOPPED_SERVICE_PID% /T /F >nul 2>&1
if errorlevel 1 exit /b 1
ping.exe 127.0.0.1 -n 3 >nul
exit /b 0

:WAIT_FOR_RUNNING
set /a WAIT_SECONDS=0
:WAIT_RUNNING_LOOP
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]4[ ]*RUNNING" >nul
if not errorlevel 1 exit /b 0
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 exit /b 1
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_RUNNING_LOOP

:CHECK_EMPTY_DIRECTORY
if not exist "%~1\" exit /b 0
dir /b /a "%~1" 2>nul | findstr.exe "." >nul
if errorlevel 1 exit /b 0
exit /b 1

:DELETE_DIRECTORY_RETRY
set "DELETE_TARGET=%~1"
if not exist "%DELETE_TARGET%\" exit /b 0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
set /a DELETE_ATTEMPT=0
:DELETE_DIRECTORY_LOOP
rmdir /S /Q "%DELETE_TARGET%" >nul 2>&1
if not exist "%DELETE_TARGET%\" exit /b 0
set /a DELETE_ATTEMPT+=1
if %DELETE_ATTEMPT% GEQ 30 goto :OFFER_DIRECTORY_DELETE_RETRY
ping.exe 127.0.0.1 -n 2 >nul
goto :DELETE_DIRECTORY_LOOP

:OFFER_DIRECTORY_DELETE_RETRY
echo.
echo Directory is still in use or access is denied:
echo   %DELETE_TARGET%
echo Close Explorer, Total Commander, database tools and other programs that
echo may have this directory open.
echo   1 - Retry deletion
echo   2 - Cancel and preserve the remaining files
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
set /a DELETE_ATTEMPT=0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
goto :DELETE_DIRECTORY_LOOP

:RESTART_OLD_SERVICE_AND_FAIL
sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED

:ROLLBACK_PREPARE_FAILED
echo ERROR: Cannot move the previous installation into rollback folders.
if exist "%ROLLBACK_INSTALL%" if not exist "%INSTALL_ROOT%" move "%ROLLBACK_INSTALL%" "%INSTALL_ROOT%" >nul
if exist "%ROLLBACK_DATA%" if not exist "%DATA_DIR%" move "%ROLLBACK_DATA%" "%DATA_DIR%" >nul
sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED

:COPY_FAILED
echo ERROR: Cannot copy installation files.
goto :ROLLBACK_OR_FAIL

:INSTALLED_CONFIG_INVALID
echo ERROR: Installed Backup.json failed validation.
goto :ROLLBACK_OR_FAIL

:SERVICE_SETUP_FAILED
echo ERROR: Cannot configure the Windows service.
goto :ROLLBACK_OR_FAIL

:SERVICE_START_FAILED
echo ERROR: The service did not reach RUNNING state within 120 seconds.
sc.exe query "%SERVICE_NAME%"
echo Check logs in %DATA_DIR%\logs
goto :ROLLBACK_OR_FAIL

:ROLLBACK_OR_FAIL
if "%ROLLBACK_READY%"=="1" goto :ROLLBACK_REINSTALL
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 goto :FAILED_WITH_FILES
call :STOP_SERVICE
sc.exe delete "%SERVICE_NAME%" >nul 2>&1
goto :FAILED_WITH_FILES

:ROLLBACK_REINSTALL
echo Restoring the previous working installation...
call :STOP_SERVICE
if errorlevel 1 goto :ROLLBACK_STOP_FAILED
rmdir /S /Q "%INSTALL_ROOT%" >nul 2>&1
rmdir /S /Q "%DATA_DIR%" >nul 2>&1
if exist "%INSTALL_ROOT%" goto :ROLLBACK_FILES_FAILED
if exist "%DATA_DIR%" goto :ROLLBACK_FILES_FAILED
move "%ROLLBACK_INSTALL%" "%INSTALL_ROOT%" >nul
move "%ROLLBACK_DATA%" "%DATA_DIR%" >nul
if not exist "%INSTALL_ROOT%" goto :ROLLBACK_FILES_FAILED
if not exist "%DATA_DIR%" goto :ROLLBACK_FILES_FAILED
set "ROLLBACK_READY=0"
sc.exe start "%SERVICE_NAME%" >nul 2>&1
echo Previous application and data files were restored.
goto :FAILED

:ROLLBACK_STOP_FAILED
echo ERROR: The new service could not be stopped for automatic rollback.
echo Both rollback folders were preserved:
echo   %ROLLBACK_INSTALL%
echo   %ROLLBACK_DATA%
goto :FAILED

:ROLLBACK_FILES_FAILED
echo ERROR: Automatic rollback could not replace the new directories.
echo Rollback data was preserved where possible:
echo   %ROLLBACK_INSTALL%
echo   %ROLLBACK_DATA%
goto :FAILED

:NOT_ADMIN
echo ERROR: Run Install-BackupService.bat as Administrator.
goto :FAILED

:UNKNOWN_ARGUMENT
echo ERROR: Unknown argument "%BAD_ARG%".
echo Supported: /validate, /SkipVcRedist, and an optional instance id.
goto :FAILED

:PACKAGE_MISSING
echo ERROR: The portable package is incomplete. Copy the entire folder again.
goto :FAILED
:PACKAGE_DAMAGED
echo ERROR: Package verification failed. Copy the entire folder again.
goto :FAILED
:CONFIG_INVALID
echo ERROR: data\Backup.json is invalid. Edit paths or restore the package.
echo Run: app\BackupService.exe --validate-config --config data\Backup.json --data-dir data
goto :FAILED
:INSTALL_LEFTOVER_DELETE_FAILED
echo ERROR: Leftover application directory could not be deleted:
echo   %INSTALL_ROOT%
goto :FAILED
:DATA_LEFTOVER_DELETE_FAILED
echo ERROR: Leftover data directory could not be deleted:
echo   %DATA_DIR%
goto :FAILED
:FAILED_WITH_FILES
echo Partial files were preserved for diagnostics:
echo   %INSTALL_ROOT%
echo   %DATA_DIR%
:FAILED
echo.
echo Installation failed. Read the error above.
pause
exit /b 1
:CANCELLED
echo Installation cancelled. No installed files were changed.
exit /b 1
