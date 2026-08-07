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
> "%UNINSTALL_LOG%" echo SearchEngineService uninstall diagnostic log
>> "%UNINSTALL_LOG%" echo Started: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Package: %PACKAGE_ROOT%

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    echo Log: %UNINSTALL_LOG%
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Uninstall-SearchEngineService.bat as Administrator.
    echo Log: %UNINSTALL_LOG%
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

if not "%~1"=="" goto :INSTANCE_READY
"%HELPER%" choose-installed-instance --output "%INSTANCE_TEMP%"
if errorlevel 3 goto :NO_INSTALLED_SERVICES
if errorlevel 2 goto :CANCELLED
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "SELECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if not defined SELECTED_instance goto :HELPER_FAILED
set "SERVICE_INSTANCE=%SELECTED_instance%"

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
set "DATA_DIR=%ProgramData%\%SERVICE_NAME%"

if /I "%TARGET_ARCH%"=="x86" goto :SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramW6432%"
if "%PROGRAM_ROOT%"=="" set "PROGRAM_ROOT=%ProgramFiles%"
goto :ROOT_READY
:SET_X86_ROOT
set "PROGRAM_ROOT=%ProgramFiles%"
if not "%ProgramFiles(x86)%"=="" set "PROGRAM_ROOT=%ProgramFiles(x86)%"
:ROOT_READY
set "INSTALL_ROOT=%PROGRAM_ROOT%\%SERVICE_NAME%"
set "UNINSTALL_STAGE=target selected"
>> "%UNINSTALL_LOG%" echo Instance: %SERVICE_INSTANCE%
>> "%UNINSTALL_LOG%" echo Service: %SERVICE_NAME%
>> "%UNINSTALL_LOG%" echo Application: %INSTALL_ROOT%
>> "%UNINSTALL_LOG%" echo Data: %DATA_DIR%

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if not errorlevel 1 set "SERVICE_EXISTS=1"
if exist "%INSTALL_ROOT%" set "FILES_EXIST=1"
if exist "%DATA_DIR%" set "FILES_EXIST=1"
if not defined SERVICE_EXISTS if not defined FILES_EXIST goto :NOT_INSTALLED

call :CHOOSE_BACKUP
if errorlevel 1 goto :CANCELLED
echo.
echo This will remove the Windows service, firewall rule, application files,
echo settings, indexes, databases, messages and logs from this computer.
echo   1 - Continue uninstall
echo   2 - Cancel
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 goto :CANCELLED

if not defined SERVICE_EXISTS goto :BACKUP
set "UNINSTALL_STAGE=stopping service"
>> "%UNINSTALL_LOG%" echo Stage: stopping service
echo Stopping %SERVICE_NAME%...
call :STOP_SERVICE
if errorlevel 1 goto :FAILED
>> "%UNINSTALL_LOG%" echo Stage: service stopped

:BACKUP
if "%BACKUP_MODE%"=="none" goto :DELETE_SERVICE
echo Exporting previous files...
set "UNINSTALL_STAGE=creating backup"
>> "%UNINSTALL_LOG%" echo Stage: creating backup
"%HELPER%" backup --install-root "%INSTALL_ROOT%" --data-dir "%DATA_DIR%" --destination "%BACKUP_DESTINATION%" --mode %BACKUP_MODE%
if errorlevel 1 goto :BACKUP_FAILED

:DELETE_SERVICE
:DELETE_FILES
cd /d "%TEMP%"
set "UNINSTALL_STAGE=deleting application directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting application directory
echo Deleting application directory...
call :DELETE_DIRECTORY_RETRY "%INSTALL_ROOT%"
if errorlevel 1 goto :APP_DIR_DELETE_FAILED
set "UNINSTALL_STAGE=deleting data directory"
>> "%UNINSTALL_LOG%" echo Stage: deleting data directory
echo Deleting data directory...
call :DELETE_DIRECTORY_RETRY "%DATA_DIR%"
if errorlevel 1 goto :DATA_DIR_DELETE_FAILED
set "UNINSTALL_STAGE=deleting rollback directories"
>> "%UNINSTALL_LOG%" echo Stage: deleting rollback directories
echo Deleting rollback directories...
call :DELETE_ROLLBACK_DIRECTORIES "%INSTALL_ROOT%.rollback-*"
if errorlevel 1 goto :APP_ROLLBACK_DELETE_FAILED
call :DELETE_ROLLBACK_DIRECTORIES "%DATA_DIR%.rollback-*"
if errorlevel 1 goto :DATA_ROLLBACK_DELETE_FAILED

if not defined SERVICE_EXISTS goto :DELETE_FIREWALL
set "UNINSTALL_STAGE=deleting service registration"
>> "%UNINSTALL_LOG%" echo Stage: deleting service registration
echo Deleting service registration...
call :DELETE_SERVICE_RETRY
if errorlevel 1 goto :SERVICE_DELETE_FAILED

:DELETE_FIREWALL
set "UNINSTALL_STAGE=deleting firewall rule"
netsh.exe advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1

echo.
echo %SERVICE_NAME% was completely removed.
if not "%BACKUP_MODE%"=="none" echo The requested backup was created before deletion.
>> "%UNINSTALL_LOG%" echo Completed successfully: %DATE% %TIME%
echo Log: %UNINSTALL_LOG%
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WAIT_BEFORE_CLOSE
exit /b 0

:CHOOSE_BACKUP
echo Backup before uninstall:
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
echo.
echo WARNING: no backup will be available after deletion.
echo   1 - Cancel ^(recommended^)
echo   2 - Continue without backup
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 0
exit /b 1

:STOP_SERVICE
set "STOPPED_SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /I "PID"') do set "STOPPED_SERVICE_PID=%%P"
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
echo The service did not stop within 120 seconds.
echo   1 - Force-terminate its process and continue
echo   2 - Cancel without deleting files
choice.exe /C 12 /N /M "Select: "
if errorlevel 2 exit /b 1
set "SERVICE_PID="
for /f "tokens=2 delims=:" %%P in ('sc.exe queryex "%SERVICE_NAME%" ^| findstr.exe /I "PID"') do set "SERVICE_PID=%%P"
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
echo.
echo Windows still keeps the service registration open:
echo   %SERVICE_NAME%
echo Close the Services window ^(services.msc^), Event Viewer and other
echo management consoles, then retry.
echo   1 - Retry the registration check
echo   2 - Cancel; the service remains stopped
choice.exe /C 12 /N /M "Select: "
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
echo %SERVICE_NAME% and its files are not installed.
echo Log: %UNINSTALL_LOG%
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WAIT_BEFORE_CLOSE
exit /b 0
:NO_INSTALLED_SERVICES
del /Q "%INSTANCE_TEMP%" >nul 2>&1
echo No registered SearchEngine services were found. Nothing was deleted.
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 0
:CANCELLED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
>> "%UNINSTALL_LOG%" echo Cancelled: %DATE% %TIME%
>> "%UNINSTALL_LOG%" echo Stage: %UNINSTALL_STAGE%
echo Uninstall cancelled. Nothing was deleted.
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 1
:BACKUP_FAILED
echo ERROR: Backup failed. Nothing was deleted.
if defined SERVICE_EXISTS sc.exe start "%SERVICE_NAME%" >nul 2>&1
goto :FAILED
:APP_DIR_DELETE_FAILED
echo ERROR: Application directory could not be deleted:
echo   %INSTALL_ROOT%
goto :FAILED
:DATA_DIR_DELETE_FAILED
echo ERROR: Data directory could not be deleted:
echo   %DATA_DIR%
goto :FAILED
:APP_ROLLBACK_DELETE_FAILED
echo ERROR: An application rollback directory could not be deleted.
goto :FAILED
:DATA_ROLLBACK_DELETE_FAILED
echo ERROR: A data rollback directory could not be deleted.
goto :FAILED
:SERVICE_DELETE_FAILED
echo ERROR: Files were deleted, but the service registration remains.
echo Run this uninstaller again for %SERVICE_NAME%.
goto :FAILED
:FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
call :WRITE_FAILURE_DIAGNOSTICS
echo Uninstall failed. Undeleted files were preserved.
echo Failed stage: %UNINSTALL_STAGE%
echo Log: %UNINSTALL_LOG%
call :WAIT_BEFORE_CLOSE
exit /b 1

:HELPER_FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
echo ERROR: SearchEngineConfig could not enumerate installed services.
goto :FAILED

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
echo Log: %UNINSTALL_LOG%
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
if exist "%INSTALL_ROOT%\" dir /A "%INSTALL_ROOT%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%INSTALL_ROOT%\" >> "%UNINSTALL_LOG%" echo MISSING
>> "%UNINSTALL_LOG%" echo Data directory:
if exist "%DATA_DIR%\" dir /A "%DATA_DIR%" >> "%UNINSTALL_LOG%" 2>&1
if not exist "%DATA_DIR%\" >> "%UNINSTALL_LOG%" echo MISSING
exit /b 0

:WAIT_BEFORE_CLOSE
echo.
choice.exe /C 0 /N /M "Press 0 to close: "
exit /b 0
