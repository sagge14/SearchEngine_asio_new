@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "SERVICE_INSTANCE=default"
if exist "%PACKAGE_ROOT%ServiceInstance.cmd" call "%PACKAGE_ROOT%ServiceInstance.cmd"
set "VALIDATE_ONLY=0"
set "UI_LANGUAGE=auto"
if /I "%~1"=="/validate" set "VALIDATE_ONLY=1"
if not "%~1"=="" if /I not "%~1"=="/validate" set "SERVICE_INSTANCE=%~1"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "HELPER=%PACKAGE_ROOT%tools\SearchEngineConfig.exe"
set "SETTINGS_TEMPLATE=%PACKAGE_ROOT%data\Settings.json"
set "VC_REDIST=%PACKAGE_ROOT%prerequisites\{{VC_REDIST_FILE}}"
set "INSTANCE_TEMP=%TEMP%\SearchEngineService-Instance-%RANDOM%-%RANDOM%.txt"

if not exist "%PACKAGE_ROOT%app\SearchEngine.exe" goto :PACKAGE_MISSING
if not exist "%HELPER%" goto :PACKAGE_MISSING
if not exist "%PACKAGE_ROOT%ServiceInstance.cmd" goto :PACKAGE_MISSING
if not exist "%SETTINGS_TEMPLATE%" goto :PACKAGE_MISSING
if not exist "%PACKAGE_ROOT%data\OEM866.INI" goto :PACKAGE_MISSING
if not exist "%VC_REDIST%" goto :PACKAGE_MISSING
if not exist "%PACKAGE_ROOT%Verify-Package.bat" goto :PACKAGE_MISSING

call "%PACKAGE_ROOT%Verify-Package.bat" /quiet
if errorlevel 1 goto :PACKAGE_DAMAGED

if "%VALIDATE_ONLY%"=="1" goto :INSTANCE_READY
if not "%~1"=="" goto :INSTANCE_READY
"%HELPER%" choose-instance --default "%SERVICE_INSTANCE%" --output "%INSTANCE_TEMP%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "SELECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if not defined SELECTED_instance goto :HELPER_FAILED
if not defined SELECTED_language goto :HELPER_FAILED
set "SERVICE_INSTANCE=%SELECTED_instance%"
set "UI_LANGUAGE=%SELECTED_language%"

:INSTANCE_READY
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
set "CONFIG_TEMP=%TEMP%\%SERVICE_NAME%-Settings-%RANDOM%-%RANDOM%.json"
set "HELPER_OUTPUT=%TEMP%\%SERVICE_NAME%-Helper-%RANDOM%-%RANDOM%.txt"
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
set "INSTALL_ROOT=%PROGRAM_ROOT%\%SERVICE_NAME%"
set "INSTALLED_BIN=%INSTALL_ROOT%\bin"
set "INSTALLED_TOOLS=%INSTALL_ROOT%\tools"

echo SearchEngineService portable installer ^(%TARGET_ARCH%^)
echo Instance: %SERVICE_INSTANCE% ^(%SERVICE_NAME%^)
echo.

"%HELPER%" validate --settings "%SETTINGS_TEMPLATE%" >nul
if errorlevel 1 goto :SETTINGS_INVALID

"%HELPER%" system-info > "%HELPER_OUTPUT%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do set "SYS_%%A=%%B"
"%HELPER%" inspect --settings "%SETTINGS_TEMPLATE%" > "%HELPER_OUTPUT%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do set "TPL_%%A=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined SYS_recommended_threads goto :HELPER_FAILED
if not defined TPL_port goto :HELPER_FAILED

if "%VALIDATE_ONLY%"=="1" goto :VALIDATED

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 goto :NOT_ADMIN

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 goto :CLEAN_DESTINATION_CHECK
set "REINSTALL=1"
"%HELPER%" inspect --settings "%DATA_DIR%\Settings.json" > "%HELPER_OUTPUT%" 2>nul
if errorlevel 1 goto :OLD_SETTINGS_INSPECTED
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do set "OLD_%%A=%%B"
:OLD_SETTINGS_INSPECTED
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo An installed SearchEngineService was found.
echo   1 - Reinstall or update it ^(recommended^)
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED
call :CHOOSE_BACKUP
if errorlevel 1 goto :CANCELLED
goto :CONFIGURE

:CLEAN_DESTINATION_CHECK
set "LEFTOVER_DIRECTORIES=0"
call :CHECK_EMPTY_DIRECTORY "%INSTALL_ROOT%"
if errorlevel 1 set "LEFTOVER_DIRECTORIES=1"
call :CHECK_EMPTY_DIRECTORY "%DATA_DIR%"
if errorlevel 1 set "LEFTOVER_DIRECTORIES=1"
if "%LEFTOVER_DIRECTORIES%"=="0" goto :CONFIGURE

echo.
echo Files remain from an earlier incomplete uninstall, but the Windows
echo service %SERVICE_NAME% is not registered.
if exist "%INSTALL_ROOT%\" echo   Application: %INSTALL_ROOT%
if exist "%DATA_DIR%\" echo   Data:        %DATA_DIR%
echo.
echo Removing these folders permanently deletes their settings, indexes,
echo databases, messages and logs.
echo   1 - Delete the leftover folders and continue installation
echo   2 - Cancel ^(recommended if the files must be preserved^)
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED
call :DELETE_DIRECTORY_RETRY "%INSTALL_ROOT%"
if errorlevel 1 goto :INSTALL_LEFTOVER_DELETE_FAILED
call :DELETE_DIRECTORY_RETRY "%DATA_DIR%"
if errorlevel 1 goto :DATA_LEFTOVER_DELETE_FAILED

:CONFIGURE
if "%REINSTALL%"=="1" goto :CONFIGURE_WITH_IMPORT
"%HELPER%" configure-interactive --template "%SETTINGS_TEMPLATE%" --output "%CONFIG_TEMP%" --language "%UI_LANGUAGE%"
if errorlevel 1 goto :HELPER_FAILED
goto :CONFIGURED

:CONFIGURE_WITH_IMPORT
if not exist "%DATA_DIR%\Settings.json" goto :CONFIGURE_WITHOUT_OLD_SETTINGS
"%HELPER%" configure-interactive --template "%SETTINGS_TEMPLATE%" --import-settings "%DATA_DIR%\Settings.json" --output "%CONFIG_TEMP%" --language "%UI_LANGUAGE%"
if errorlevel 1 goto :HELPER_FAILED
goto :CONFIGURED

:CONFIGURE_WITHOUT_OLD_SETTINGS
"%HELPER%" configure-interactive --template "%SETTINGS_TEMPLATE%" --output "%CONFIG_TEMP%" --language "%UI_LANGUAGE%"
if errorlevel 1 goto :HELPER_FAILED

:CONFIGURED
"%HELPER%" inspect --settings "%CONFIG_TEMP%" > "%HELPER_OUTPUT%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%HELPER_OUTPUT%") do set "CFG_%%A=%%B"
del /Q "%HELPER_OUTPUT%" >nul 2>&1
if not defined CFG_port goto :HELPER_FAILED
if not defined CFG_year goto :HELPER_FAILED
if not defined CFG_threads goto :HELPER_FAILED
if not defined CFG_file_timeout_sec goto :HELPER_FAILED
if not defined CFG_prm_short_content_autodetect goto :HELPER_FAILED
set "SERVICE_PORT=%CFG_port%"
set "SERVICE_YEAR=%CFG_year%"
set "SERVICE_THREADS=%CFG_threads%"
set "FILE_TIMEOUT=%CFG_file_timeout_sec%"
set "PRM_AUTODETECT=%CFG_prm_short_content_autodetect%"
"%HELPER%" validate --settings "%CONFIG_TEMP%" >nul
if errorlevel 1 goto :HELPER_FAILED

:DIRECTORIES_VALID
echo.
echo Selected configuration:
echo   Port:                 %SERVICE_PORT%
echo   Year:                 %SERVICE_YEAR%
echo   Executor threads:     %SERVICE_THREADS%
echo   One-file timeout:     %FILE_TIMEOUT% sec
if "%PRM_AUTODETECT%"=="1" echo   PRM short content:    enabled
if "%PRM_AUTODETECT%"=="0" echo   PRM short content:    disabled
echo.

echo [1/8] Installing Microsoft Visual C++ Runtime...
start "" /wait "%VC_REDIST%" /install /quiet /norestart
set "REDIST_EXIT=%ERRORLEVEL%"
if "%REDIST_EXIT%"=="0" goto :REDIST_OK
if "%REDIST_EXIT%"=="1638" goto :REDIST_OK
if "%REDIST_EXIT%"=="3010" goto :REDIST_RESTART
echo Visual C++ Runtime setup failed with exit code %REDIST_EXIT%.
goto :FAILED

:REDIST_RESTART
echo WARNING: Windows must be restarted after the installation.

:REDIST_OK
if "%REINSTALL%"=="0" goto :CHECK_NEW_PORT
echo [2/8] Stopping the installed service...
call :STOP_SERVICE
if errorlevel 1 goto :FAILED

if "%BACKUP_MODE%"=="none" goto :PREPARE_ROLLBACK
echo [3/8] Exporting the previous installation...
"%HELPER%" backup --install-root "%INSTALL_ROOT%" --data-dir "%DATA_DIR%" --destination "%BACKUP_DESTINATION%" --mode %BACKUP_MODE%
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
goto :CHECK_NEW_PORT

:CHECK_NEW_PORT
"%HELPER%" check-port --port %SERVICE_PORT% >nul
if errorlevel 1 goto :PORT_IN_USE

echo [4/8] Copying application files...
md "%INSTALLED_BIN%" >nul 2>&1
md "%INSTALLED_TOOLS%" >nul 2>&1
md "%DATA_DIR%" >nul 2>&1
if not exist "%INSTALLED_BIN%\" goto :COPY_FAILED
if not exist "%INSTALLED_TOOLS%\" goto :COPY_FAILED
if not exist "%DATA_DIR%\" goto :COPY_FAILED
xcopy.exe "%PACKAGE_ROOT%app\*" "%INSTALLED_BIN%\" /E /I /H /R /Y >nul
if errorlevel 1 goto :COPY_FAILED
xcopy.exe "%PACKAGE_ROOT%tools\*" "%INSTALLED_TOOLS%\" /E /I /H /R /Y >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%README.txt" "%INSTALL_ROOT%\README.txt" >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%INSTALLATION_GUIDE_RU.txt" "%INSTALL_ROOT%\INSTALLATION_GUIDE_RU.txt" >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%PACKAGE_ROOT%ServiceInstance.cmd" "%INSTALL_ROOT%\ServiceInstance.cmd" >nul
if errorlevel 1 goto :COPY_FAILED

echo [5/8] Copying data and generated settings...
xcopy.exe "%PACKAGE_ROOT%data\*" "%DATA_DIR%\" /E /I /H /R /Y >nul
if errorlevel 1 goto :COPY_FAILED
copy /Y "%CONFIG_TEMP%" "%DATA_DIR%\Settings.json" >nul
if errorlevel 1 goto :COPY_FAILED
> "%DATA_DIR%\client-endpoint.txt" echo server_id=%SERVICE_INSTANCE%
>> "%DATA_DIR%\client-endpoint.txt" echo display_name=%DISPLAY_NAME%
>> "%DATA_DIR%\client-endpoint.txt" echo host=%COMPUTERNAME%
>> "%DATA_DIR%\client-endpoint.txt" echo god=%SERVICE_YEAR%
>> "%DATA_DIR%\client-endpoint.txt" echo port=%SERVICE_PORT%
>> "%DATA_DIR%\client-endpoint.txt" echo service_name=%SERVICE_NAME%
if errorlevel 1 goto :COPY_FAILED

echo [6/8] Registering and configuring the Windows service...
if "%REINSTALL%"=="1" goto :CONFIG_EXISTING_SERVICE
sc.exe create "%SERVICE_NAME%" binPath= "\"%INSTALLED_BIN%\SearchEngine.exe\" --service --service-name \"%SERVICE_NAME%\" --data-dir \"%DATA_DIR%\"" start= delayed-auto DisplayName= "%DISPLAY_NAME%" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
goto :CONFIGURE_SERVICE_COMMON

:CONFIG_EXISTING_SERVICE
sc.exe config "%SERVICE_NAME%" binPath= "\"%INSTALLED_BIN%\SearchEngine.exe\" --service --service-name \"%SERVICE_NAME%\" --data-dir \"%DATA_DIR%\"" start= delayed-auto DisplayName= "%DISPLAY_NAME%" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED

:CONFIGURE_SERVICE_COMMON
sc.exe description "%SERVICE_NAME%" "ASIO search and indexing server; instance=%SERVICE_INSTANCE%" >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
sc.exe failure "%SERVICE_NAME%" reset= 86400 actions= restart/60000/restart/60000/restart/300000 >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
sc.exe failureflag "%SERVICE_NAME%" 1 >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" /v PreshutdownTimeout /t REG_DWORD /d 180000 /f >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED

echo [7/8] Configuring Windows Firewall for TCP port %SERVICE_PORT%...
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1
netsh.exe advfirewall firewall add rule name="%FIREWALL_RULE%" dir=in action=allow protocol=TCP localport=%SERVICE_PORT% program="%INSTALLED_BIN%\SearchEngine.exe" enable=yes >nul
if errorlevel 1 goto :SERVICE_SETUP_FAILED

echo [8/8] Starting the service and checking PING/PONG...
sc.exe start "%SERVICE_NAME%" >nul
if errorlevel 1 goto :SERVICE_START_FAILED
call :WAIT_FOR_RUNNING
if errorlevel 1 goto :SERVICE_START_FAILED
call :WAIT_FOR_HEALTH
if errorlevel 1 goto :SERVICE_HEALTH_FAILED

if "%ROLLBACK_READY%"=="0" goto :INSTALLED
rmdir /S /Q "%ROLLBACK_INSTALL%" >nul 2>&1
if exist "%ROLLBACK_INSTALL%" echo WARNING: old application directory could not be removed: %ROLLBACK_INSTALL%
rmdir /S /Q "%ROLLBACK_DATA%" >nul 2>&1
if exist "%ROLLBACK_DATA%" echo WARNING: old data directory could not be removed: %ROLLBACK_DATA%
set "ROLLBACK_READY=0"

:INSTALLED
del /Q "%CONFIG_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo.
echo Installation completed successfully.
echo Service:     %SERVICE_NAME% ^(RUNNING and PING/PONG OK^)
echo Application: %INSTALLED_BIN%
echo Data:        %DATA_DIR%
echo Logs:        %DATA_DIR%\logs
echo Client hint: %DATA_DIR%\client-endpoint.txt
echo.
pause
exit /b 0

:VALIDATED
echo Package and Settings.json validation completed successfully.
exit /b 0

:CHOOSE_BACKUP
echo.
echo Backup before replacing the installed files:
echo   1 - Full application and data backup ^(recommended^)
echo   2 - Settings and logs only
echo   3 - Do not create a backup
choice.exe /C 123 /N /M "Select: "
if errorlevel 3 goto :CONFIRM_NO_BACKUP
if errorlevel 2 set "BACKUP_MODE=settings-logs"
if errorlevel 2 goto :READ_BACKUP_DESTINATION
set "BACKUP_MODE=full"
:READ_BACKUP_DESTINATION
set "BACKUP_DESTINATION="
set /p "BACKUP_DESTINATION=Destination disk or folder, for example E:\Backups: "
if "%BACKUP_DESTINATION%"=="" goto :READ_BACKUP_DESTINATION
exit /b 0
:CONFIRM_NO_BACKUP
echo No backup means that old settings, indexes and logs will be deleted
echo only after the new service passes its health check.
echo   1 - Cancel ^(recommended^)
echo   2 - Continue without backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 set "BACKUP_MODE=none"
if errorlevel 2 exit /b 0
exit /b 1

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

:WAIT_FOR_HEALTH
set /a HEALTH_ATTEMPTS=0
:HEALTH_LOOP
"%HELPER%" health --port %SERVICE_PORT% --timeout-ms 10000 >nul 2>&1
if not errorlevel 1 exit /b 0
set /a HEALTH_ATTEMPTS+=1
if %HEALTH_ATTEMPTS% GEQ 12 exit /b 1
ping.exe 127.0.0.1 -n 3 >nul
goto :HEALTH_LOOP

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
echo ERROR: Cannot move the previous installation into rollback directories.
if exist "%ROLLBACK_INSTALL%" if not exist "%INSTALL_ROOT%" move "%ROLLBACK_INSTALL%" "%INSTALL_ROOT%" >nul
if exist "%ROLLBACK_DATA%" if not exist "%DATA_DIR%" move "%ROLLBACK_DATA%" "%DATA_DIR%" >nul
sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED

:PORT_IN_USE
echo ERROR: TCP port %SERVICE_PORT% is already occupied by another process.
goto :ROLLBACK_OR_FAIL

:COPY_FAILED
echo ERROR: Cannot copy installation files.
goto :ROLLBACK_OR_FAIL

:SERVICE_SETUP_FAILED
echo ERROR: Cannot configure the Windows service.
goto :ROLLBACK_OR_FAIL

:SERVICE_START_FAILED
echo ERROR: The service did not reach RUNNING state within 120 seconds.
sc.exe query "%SERVICE_NAME%"
goto :ROLLBACK_OR_FAIL

:SERVICE_HEALTH_FAILED
echo ERROR: The service process is running but did not answer PING within the timeout.
echo Check logs in %DATA_DIR%\logs
goto :ROLLBACK_OR_FAIL

:ROLLBACK_OR_FAIL
if "%ROLLBACK_READY%"=="1" goto :ROLLBACK_REINSTALL
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 goto :CLEAN_FAILURE_FIREWALL
call :STOP_SERVICE
sc.exe delete "%SERVICE_NAME%" >nul 2>&1
:CLEAN_FAILURE_FIREWALL
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1
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
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1
if defined OLD_port netsh.exe advfirewall firewall add rule name="%FIREWALL_RULE%" dir=in action=allow protocol=TCP localport=%OLD_port% program="%INSTALLED_BIN%\SearchEngine.exe" enable=yes >nul 2>&1
sc.exe start "%SERVICE_NAME%" >nul 2>&1
echo Previous application and data files were restored.
goto :FAILED

:ROLLBACK_STOP_FAILED
echo ERROR: The new service could not be stopped for automatic rollback.
echo Both rollback directories were preserved:
echo   %ROLLBACK_INSTALL%
echo   %ROLLBACK_DATA%
goto :FAILED

:ROLLBACK_FILES_FAILED
echo ERROR: Automatic rollback could not replace the new directories.
echo Rollback data was preserved where possible:
echo   %ROLLBACK_INSTALL%
echo   %ROLLBACK_DATA%
goto :FAILED

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
pause
exit /b 1

:NOT_ADMIN
echo ERROR: Run Install-SearchEngineService.bat as Administrator.
goto :FAILED
:PACKAGE_MISSING
echo ERROR: The portable package is incomplete. Copy the entire folder again.
goto :FAILED
:PACKAGE_DAMAGED
echo ERROR: Package verification failed. Copy the entire folder again.
goto :FAILED
:SETTINGS_INVALID
echo ERROR: data\Settings.json is invalid. Run the helper validation or restore the package.
goto :FAILED
:HELPER_FAILED
echo ERROR: SearchEngineConfig could not validate or generate settings.
goto :FAILED
:INSTALL_LEFTOVER_DELETE_FAILED
echo ERROR: Leftover application directory could not be deleted: %INSTALL_ROOT%
goto :FAILED
:DATA_LEFTOVER_DELETE_FAILED
echo ERROR: Leftover data directory could not be deleted: %DATA_DIR%
goto :FAILED
:FAILED_WITH_FILES
echo Partial files were preserved for diagnostics:
echo   %INSTALL_ROOT%
echo   %DATA_DIR%
:FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
del /Q "%CONFIG_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo.
echo Installation failed. Read the error above.
pause
exit /b 1
:CANCELLED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
del /Q "%CONFIG_TEMP%" >nul 2>&1
del /Q "%HELPER_OUTPUT%" >nul 2>&1
echo Installation cancelled. No installed files were changed.
exit /b 1
