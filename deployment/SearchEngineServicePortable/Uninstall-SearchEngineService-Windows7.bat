@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "SERVICE_INSTANCE=default"
if exist "%PACKAGE_ROOT%ServiceInstance.cmd" call "%PACKAGE_ROOT%ServiceInstance.cmd"
if not "%~1"=="" set "SERVICE_INSTANCE=%~1"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "HELPER=%PACKAGE_ROOT%tools\SearchEngineConfig.exe"
set "INSTANCE_TEMP=%TEMP%\SearchEngineService-Uninstall-%RANDOM%-%RANDOM%.txt"
set "UNINSTALL_LOG=%TEMP%\SearchEngineService-Uninstall-last.log"
set "UNINSTALL_STAGE=initialization"
set "BACKUP_MODE=none"
set "UI_LANGUAGE=auto"
> "%UNINSTALL_LOG%" echo SearchEngineService uninstall diagnostic log
>> "%UNINSTALL_LOG%" echo Started: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Package: %PACKAGE_ROOT%

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    echo Log: %UNINSTALL_LOG%
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

if not "%~1"=="" goto :SELECT_LANGUAGE_ONLY
"%HELPER%" choose-installed-instance --output "%INSTANCE_TEMP%"
set "PICKER_EXIT=%ERRORLEVEL%"
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "SELECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if defined SELECTED_language set "UI_LANGUAGE=%SELECTED_language%"
if "%PICKER_EXIT%"=="3" goto :NO_INSTALLED_SERVICES
if "%PICKER_EXIT%"=="2" goto :CANCELLED
if not "%PICKER_EXIT%"=="0" goto :HELPER_FAILED
if not defined SELECTED_instance goto :HELPER_FAILED
if not defined SELECTED_language goto :HELPER_FAILED
set "SERVICE_INSTANCE=%SELECTED_instance%"
goto :LANGUAGE_READY

:SELECT_LANGUAGE_ONLY
"%HELPER%" choose-language --output "%INSTANCE_TEMP%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "SELECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if not defined SELECTED_language goto :HELPER_FAILED
set "UI_LANGUAGE=%SELECTED_language%"

:LANGUAGE_READY
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    call :UI uninstall.not_admin
    call :UI common.log_path "%UNINSTALL_LOG%"
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

:INSTANCE_READY
echo(%SERVICE_INSTANCE%| findstr.exe /R /X "[A-Za-z0-9][A-Za-z0-9_-]*" >nul
if errorlevel 1 goto :INVALID_INSTANCE
if not "%SERVICE_INSTANCE:~32,1%"=="" goto :INVALID_INSTANCE
if /I "%SERVICE_INSTANCE%"=="default" (
    set "SERVICE_NAME=SearchEngineService"
) else (
    set "SERVICE_NAME=SearchEngineService-%SERVICE_INSTANCE%"
)
set "FIREWALL_RULE=%SERVICE_NAME% TCP"
set "STANDARD_DATA_DIR=%ProgramData%\%SERVICE_NAME%"
set "DATA_DIR=%STANDARD_DATA_DIR%"

if /I "%TARGET_ARCH%"=="x86" goto :SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramW6432%"
if "%PROGRAM_ROOT%"=="" set "PROGRAM_ROOT=%ProgramFiles%"
goto :ROOT_READY
:SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramFiles%"
if not "%ProgramFiles(x86)%"=="" set "PROGRAM_ROOT=%ProgramFiles(x86)%"
:ROOT_READY
set "STANDARD_INSTALL_ROOT=%PROGRAM_ROOT%\%SERVICE_NAME%"
set "INSTALL_ROOT=%STANDARD_INSTALL_ROOT%"
set "ARCHIVE_ROOT="
set "DELETE_APP_ROOT=%INSTALL_ROOT%"
set "BACKUP_INSTALL_ROOT=%INSTALL_ROOT%"
set "BACKUP_DATA_DIR=%DATA_DIR%"
set "UNINSTALL_STAGE=target selected"

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 goto :ACTUAL_PATHS_READY
set "SERVICE_EXISTS=1"
"%HELPER%" inspect-installed --instance "%SERVICE_INSTANCE%" > "%INSTANCE_TEMP%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "INSPECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if not defined INSPECTED_install_root goto :HELPER_FAILED
if not defined INSPECTED_data_dir goto :HELPER_FAILED
set "INSTALL_ROOT=%INSPECTED_install_root%"
set "DATA_DIR=%INSPECTED_data_dir%"
if defined INSPECTED_archive_directory set "ARCHIVE_ROOT=%INSPECTED_archive_directory%"

:ACTUAL_PATHS_READY
set "DELETE_APP_ROOT=%INSTALL_ROOT%"
set "BACKUP_INSTALL_ROOT=%INSTALL_ROOT%"
set "BACKUP_DATA_DIR=%DATA_DIR%"
if defined ARCHIVE_ROOT set "DELETE_APP_ROOT=%ARCHIVE_ROOT%"
if defined ARCHIVE_ROOT set "BACKUP_INSTALL_ROOT=%ARCHIVE_ROOT%"
>> "%UNINSTALL_LOG%" echo Instance: %SERVICE_INSTANCE%
>> "%UNINSTALL_LOG%" echo Service: %SERVICE_NAME%
>> "%UNINSTALL_LOG%" echo Application: %INSTALL_ROOT%
>> "%UNINSTALL_LOG%" echo Data: %DATA_DIR%
>> "%UNINSTALL_LOG%" echo Complete application cleanup root: %DELETE_APP_ROOT%
if defined ARCHIVE_ROOT >> "%UNINSTALL_LOG%" echo Active archive: %ARCHIVE_ROOT%
>> "%UNINSTALL_LOG%" echo Standard application cleanup root: %STANDARD_INSTALL_ROOT%
>> "%UNINSTALL_LOG%" echo Standard data cleanup root: %STANDARD_DATA_DIR%

if exist "%DELETE_APP_ROOT%" set "FILES_EXIST=1"
if exist "%DATA_DIR%" set "FILES_EXIST=1"
if exist "%STANDARD_INSTALL_ROOT%" set "FILES_EXIST=1"
if exist "%STANDARD_DATA_DIR%" set "FILES_EXIST=1"
if not defined SERVICE_EXISTS if not defined FILES_EXIST goto :NOT_INSTALLED

call :CHOOSE_BACKUP
if errorlevel 1 goto :CANCELLED
call :UI uninstall.confirm
call :CHOICE 12
if errorlevel 2 goto :CANCELLED

if not defined SERVICE_EXISTS goto :BACKUP
set "UNINSTALL_STAGE=stopping service"
>> "%UNINSTALL_LOG%" echo Stage: stopping service
call :UI uninstall.stopping "%SERVICE_NAME%"
call :STOP_SERVICE
if errorlevel 1 goto :FAILED
>> "%UNINSTALL_LOG%" echo Stage: service stopped

:BACKUP
if "%BACKUP_MODE%"=="none" goto :DELETE_SERVICE
call :UI uninstall.exporting
set "UNINSTALL_STAGE=creating backup"
>> "%UNINSTALL_LOG%" echo Stage: creating backup
"%HELPER%" backup --install-root "%BACKUP_INSTALL_ROOT%" --data-dir "%BACKUP_DATA_DIR%" --destination "%BACKUP_DESTINATION%" --mode %BACKUP_MODE%
if errorlevel 1 goto :BACKUP_FAILED

:DELETE_SERVICE
:DELETE_FILES
cd /d "%TEMP%"
set "UNINSTALL_STAGE=deleting application directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting application directory
call :UI uninstall.delete_application
set "FAILED_DELETE_PATH=%DELETE_APP_ROOT%"
call :DELETE_DIRECTORY_RETRY "%DELETE_APP_ROOT%"
if errorlevel 1 goto :APP_DIR_DELETE_FAILED
set "UNINSTALL_STAGE=deleting data directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting data directory
call :UI uninstall.delete_data
set "FAILED_DELETE_PATH=%DATA_DIR%"
call :DELETE_DIRECTORY_RETRY "%DATA_DIR%"
if errorlevel 1 goto :DATA_DIR_DELETE_FAILED
if /I "%STANDARD_INSTALL_ROOT%"=="%DELETE_APP_ROOT%" goto :STANDARD_APP_DELETED
set "FAILED_DELETE_PATH=%STANDARD_INSTALL_ROOT%"
call :DELETE_DIRECTORY_RETRY "%STANDARD_INSTALL_ROOT%"
if errorlevel 1 goto :APP_DIR_DELETE_FAILED
:STANDARD_APP_DELETED
if /I "%STANDARD_DATA_DIR%"=="%DATA_DIR%" goto :STANDARD_DATA_DELETED
set "FAILED_DELETE_PATH=%STANDARD_DATA_DIR%"
call :DELETE_DIRECTORY_RETRY "%STANDARD_DATA_DIR%"
if errorlevel 1 goto :DATA_DIR_DELETE_FAILED
:STANDARD_DATA_DELETED
set "UNINSTALL_STAGE=deleting rollback directories"
>> "%UNINSTALL_LOG%" echo Stage: deleting rollback directories
call :UI uninstall.delete_rollbacks
call :DELETE_ROLLBACK_DIRECTORIES "%INSTALL_ROOT%.rollback-*"
if errorlevel 1 goto :APP_ROLLBACK_DELETE_FAILED
call :DELETE_ROLLBACK_DIRECTORIES "%DATA_DIR%.rollback-*"
if errorlevel 1 goto :DATA_ROLLBACK_DELETE_FAILED
if /I "%STANDARD_INSTALL_ROOT%"=="%INSTALL_ROOT%" goto :STANDARD_APP_ROLLBACKS_DELETED
call :DELETE_ROLLBACK_DIRECTORIES "%STANDARD_INSTALL_ROOT%.rollback-*"
if errorlevel 1 goto :APP_ROLLBACK_DELETE_FAILED
:STANDARD_APP_ROLLBACKS_DELETED
if /I "%STANDARD_DATA_DIR%"=="%DATA_DIR%" goto :STANDARD_DATA_ROLLBACKS_DELETED
call :DELETE_ROLLBACK_DIRECTORIES "%STANDARD_DATA_DIR%.rollback-*"
if errorlevel 1 goto :DATA_ROLLBACK_DELETE_FAILED
:STANDARD_DATA_ROLLBACKS_DELETED

if not defined SERVICE_EXISTS goto :DELETE_FIREWALL
set "UNINSTALL_STAGE=deleting service registration"
>> "%UNINSTALL_LOG%" echo Stage: deleting service registration
call :UI uninstall.delete_service
call :DELETE_SERVICE_RETRY
if errorlevel 1 goto :SERVICE_DELETE_FAILED

:DELETE_FIREWALL
set "UNINSTALL_STAGE=deleting firewall rule"
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1

call :UI uninstall.success "%SERVICE_NAME%"
if not "%BACKUP_MODE%"=="none" call :UI uninstall.backup_created
>> "%UNINSTALL_LOG%" echo Completed successfully: %DATE% %TIME%
call :UI common.log_path "%UNINSTALL_LOG%"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WAIT_BEFORE_CLOSE
exit /b 0

:CHOOSE_BACKUP
call :UI uninstall.backup_menu
call :CHOICE 123
if errorlevel 3 goto :CONFIRM_NO_BACKUP
if errorlevel 2 set "BACKUP_MODE=settings-logs"
if errorlevel 2 goto :READ_BACKUP_DESTINATION
set "BACKUP_MODE=full"
:READ_BACKUP_DESTINATION
set "BACKUP_DESTINATION="
call :UI common.backup_destination
set /p "BACKUP_DESTINATION="
if "%BACKUP_DESTINATION%"=="" goto :READ_BACKUP_DESTINATION
exit /b 0
:CONFIRM_NO_BACKUP
call :UI uninstall.no_backup
call :CHOICE 12
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
:WAIT_STOPPED
sc.exe query "%SERVICE_NAME%" | findstr.exe /R /C:"[ ]1[ ]*STOPPED" >nul
if not errorlevel 1 goto :WAIT_STOPPED_PROCESS_EXIT
set /a WAIT_SECONDS+=1
if %WAIT_SECONDS% GEQ 120 goto :OFFER_FORCE_STOP
ping.exe 127.0.0.1 -n 2 >nul
goto :WAIT_STOPPED
:OFFER_FORCE_STOP
call :UI common.stop_timeout
call :CHOICE 12
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
call :UI common.stopped_process "%STOPPED_SERVICE_PID%"
call :CHOICE 12
if errorlevel 2 exit /b 1
taskkill.exe /PID %STOPPED_SERVICE_PID% /T /F >nul 2>&1
if errorlevel 1 exit /b 1
ping.exe 127.0.0.1 -n 3 >nul
exit /b 0

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
call :UI common.directory_retry "%DELETE_TARGET%"
call :CHOICE 12
if errorlevel 2 exit /b 1
set /a DELETE_ATTEMPT=0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
goto :DELETE_DIRECTORY_LOOP

:DELETE_SERVICE_RETRY
sc.exe delete "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
set /a DELETE_SERVICE_ATTEMPT=0
:DELETE_SERVICE_LOOP
reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 exit /b 0
set /a DELETE_SERVICE_ATTEMPT+=1
if %DELETE_SERVICE_ATTEMPT% GEQ 30 goto :OFFER_SERVICE_DELETE_RETRY
ping.exe 127.0.0.1 -n 2 >nul
goto :DELETE_SERVICE_LOOP

:OFFER_SERVICE_DELETE_RETRY
call :UI uninstall.service_retry "%SERVICE_NAME%"
call :CHOICE 12
if errorlevel 2 exit /b 1
sc.exe delete "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
set /a DELETE_SERVICE_ATTEMPT=0
goto :DELETE_SERVICE_LOOP

:DELETE_ROLLBACK_DIRECTORIES
set "ROLLBACK_DELETE_FAILED="
for /D %%D in ("%~1") do call :DELETE_ONE_ROLLBACK_DIRECTORY "%%~fD"
if defined ROLLBACK_DELETE_FAILED exit /b 1
exit /b 0

:DELETE_ONE_ROLLBACK_DIRECTORY
call :DELETE_DIRECTORY_RETRY "%~1"
if errorlevel 1 set "ROLLBACK_DELETE_FAILED=1"
exit /b 0

:NOT_INSTALLED
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1
call :UI uninstall.not_installed "%SERVICE_NAME%"
call :UI common.log_path "%UNINSTALL_LOG%"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WAIT_BEFORE_CLOSE
exit /b 0
:NO_INSTALLED_SERVICES
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :UI uninstall.no_services
call :UI common.log_path "%UNINSTALL_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 0
:CANCELLED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
>> "%UNINSTALL_LOG%" echo Cancelled: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Stage: %UNINSTALL_STAGE%
call :UI uninstall.cancelled
call :UI common.log_path "%UNINSTALL_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 1
:BACKUP_FAILED
call :UI uninstall.backup_failed
if defined SERVICE_EXISTS sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED
:APP_DIR_DELETE_FAILED
call :UI uninstall.app_delete_failed "%FAILED_DELETE_PATH%"
goto :FAILED
:DATA_DIR_DELETE_FAILED
call :UI uninstall.data_delete_failed "%FAILED_DELETE_PATH%"
goto :FAILED
:APP_ROLLBACK_DELETE_FAILED
call :UI uninstall.app_rollback_delete_failed
goto :FAILED
:DATA_ROLLBACK_DELETE_FAILED
call :UI uninstall.data_rollback_delete_failed
goto :FAILED
:SERVICE_DELETE_FAILED
call :UI uninstall.service_delete_failed "%SERVICE_NAME%"
goto :FAILED
:FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WRITE_FAILURE_DIAGNOSTICS
call :UI uninstall.failed
call :UI common.log_path "%UNINSTALL_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 1

:HELPER_FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if /I "%UI_LANGUAGE%"=="auto" set "UI_LANGUAGE=en"
call :UI uninstall.helper_failed
goto :FAILED

:INVALID_INSTANCE
call :UI common.invalid_instance "%SERVICE_INSTANCE%"
call :UI common.log_path "%UNINSTALL_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 1

:WRITE_FAILURE_DIAGNOSTICS
>> "%UNINSTALL_LOG%" echo.
>> "%UNINSTALL_LOG%" echo Failed: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Stage: %UNINSTALL_STAGE%
>> "%UNINSTALL_LOG%" echo Service query:
sc.exe queryex "%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
>> "%UNINSTALL_LOG%" echo Service registry key:
reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%" >> "%UNINSTALL_LOG%" 2>&1
>> "%UNINSTALL_LOG%" echo Application directory:
if exist "%DELETE_APP_ROOT%\" dir /A "%DELETE_APP_ROOT%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%DELETE_APP_ROOT%\" >> "%UNINSTALL_LOG%" echo MISSING
>> "%UNINSTALL_LOG%" echo Data directory:
if exist "%DATA_DIR%\" dir /A "%DATA_DIR%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%DATA_DIR%\" >> "%UNINSTALL_LOG%" echo MISSING
>> "%UNINSTALL_LOG%" echo Standard application directory:
if exist "%STANDARD_INSTALL_ROOT%\" dir /A "%STANDARD_INSTALL_ROOT%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%STANDARD_INSTALL_ROOT%\" >> "%UNINSTALL_LOG%" echo MISSING
>> "%UNINSTALL_LOG%" echo Standard data directory:
if exist "%STANDARD_DATA_DIR%\" dir /A "%STANDARD_DATA_DIR%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%STANDARD_DATA_DIR%\" >> "%UNINSTALL_LOG%" echo MISSING
exit /b 0

:WAIT_BEFORE_CLOSE
if /I "%UI_LANGUAGE%"=="auto" (
    choice.exe /C 0 /N /M "Press 0 to close: "
) else (
    call :UI common.press_zero
    choice.exe /C 0 /N /M ""
)
exit /b 0

:CHOICE
call :UI common.select
choice.exe /C %~1 /N /M ""
exit /b %ERRORLEVEL%

:UI
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id "%~1" --arg1 "%~2" --arg2 "%~3" --arg3 "%~4" --arg4 "%~5"
exit /b 0
