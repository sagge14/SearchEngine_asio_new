@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "HELPER=%PACKAGE_ROOT%tools\SearchEngineConfig.exe"
set "INSTANCE_TEMP=%TEMP%\SearchEngineService-RemoveAuthKeys-%RANDOM%-%RANDOM%.txt"
set "AUTHKEYS_LOG=%TEMP%\SearchEngineService-RemoveAuthKeys-last.log"
set "UI_LANGUAGE=auto"
set "HAS_ISSUER="
set "HAS_PROFILE="
set "HAS_LEGACY="
set "REMAINING_SERVICES="
set "DELETE_ISSUER="
set "DELETE_PROFILE="
set "ISSUER_ROOT=%ProgramData%\SearchClientTokenIssuer"
set "LEGACY_TOKEN=%ProgramData%\SearchEngine\searchclient-auth-token.json"
set "PROFILE_DIR="
if defined LOCALAPPDATA set "PROFILE_DIR=%LOCALAPPDATA%\SearchEngine"
> "%AUTHKEYS_LOG%" echo SearchEngineService auth-key cleanup diagnostic log
>> "%AUTHKEYS_LOG%" echo Started: %DATE% %TIME%
>> "%AUTHKEYS_LOG%" echo Package: %PACKAGE_ROOT%

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    echo Log: %AUTHKEYS_LOG%
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

"%HELPER%" choose-language --output "%INSTANCE_TEMP%"
if errorlevel 1 goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%INSTANCE_TEMP%") do set "SELECTED_%%A=%%B"
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if not defined SELECTED_language goto :HELPER_FAILED
set "UI_LANGUAGE=%SELECTED_language%"

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    call :UI authkeys.not_admin
    call :UI common.log_path "%AUTHKEYS_LOG%"
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

if exist "%ISSUER_ROOT%\" set "HAS_ISSUER=1"
if defined PROFILE_DIR if exist "%PROFILE_DIR%\" set "HAS_PROFILE=1"
if exist "%LEGACY_TOKEN%" set "HAS_LEGACY=1"
if not defined HAS_ISSUER if not defined HAS_PROFILE if not defined HAS_LEGACY (
    call :UI authkeys.nothing
    call :UI common.log_path "%AUTHKEYS_LOG%"
    call :WAIT_BEFORE_CLOSE
    exit /b 0
)

call :UI authkeys.found
if defined HAS_ISSUER call :UI authkeys.found_path "%ISSUER_ROOT%"
if defined HAS_PROFILE call :UI authkeys.found_path "%PROFILE_DIR%"
if defined HAS_LEGACY call :UI authkeys.found_path "%LEGACY_TOKEN%"

reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services" /k /f "SearchEngineService" 2>nul | findstr.exe /I /C:"SearchEngineService" >nul
if not errorlevel 1 set "REMAINING_SERVICES=1"
if defined REMAINING_SERVICES if defined HAS_ISSUER call :UI authkeys.services_remain

if not defined HAS_ISSUER goto :ASK_PROFILE
call :UI authkeys.confirm_issuer "%ISSUER_ROOT%"
call :CHOICE 12
if not errorlevel 2 goto :ASK_PROFILE
if not defined REMAINING_SERVICES (
    set "DELETE_ISSUER=1"
    goto :ASK_PROFILE
)
call :UI authkeys.confirm_issuer_again
call :CHOICE 12
if errorlevel 2 set "DELETE_ISSUER=1"

:ASK_PROFILE
if not defined HAS_PROFILE if not defined HAS_LEGACY goto :APPLY
if defined HAS_PROFILE (
    call :UI authkeys.confirm_profile "%PROFILE_DIR%"
) else (
    call :UI authkeys.confirm_profile "%LEGACY_TOKEN%"
)
if defined HAS_PROFILE if defined HAS_LEGACY call :UI authkeys.legacy_included "%LEGACY_TOKEN%"
call :CHOICE 12
if errorlevel 2 set "DELETE_PROFILE=1"

:APPLY
if not defined DELETE_ISSUER if not defined DELETE_PROFILE goto :CANCELLED

if defined DELETE_ISSUER (
    >> "%AUTHKEYS_LOG%" echo Deleting issuer: %ISSUER_ROOT%
    call :UI authkeys.deleting_issuer
    call :DELETE_DIRECTORY_RETRY "%ISSUER_ROOT%"
    if errorlevel 1 goto :ISSUER_DELETE_FAILED
)

if defined DELETE_PROFILE (
    >> "%AUTHKEYS_LOG%" echo Deleting profile: %PROFILE_DIR%
    call :UI authkeys.deleting_profile
    if defined HAS_PROFILE (
        call :DELETE_DIRECTORY_RETRY "%PROFILE_DIR%"
        if errorlevel 1 goto :PROFILE_DELETE_FAILED
    )
    if defined HAS_LEGACY (
        del /F /Q "%LEGACY_TOKEN%" >nul 2>&1
        if exist "%LEGACY_TOKEN%" goto :LEGACY_DELETE_FAILED
    )
)

call :UI authkeys.success
>> "%AUTHKEYS_LOG%" echo Completed successfully: %DATE% %TIME%
call :UI common.log_path "%AUTHKEYS_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 0

:ISSUER_DELETE_FAILED
call :UI authkeys.issuer_delete_failed "%ISSUER_ROOT%"
goto :FAILED
:PROFILE_DELETE_FAILED
call :UI authkeys.profile_delete_failed "%PROFILE_DIR%"
goto :FAILED
:LEGACY_DELETE_FAILED
call :UI authkeys.legacy_delete_failed "%LEGACY_TOKEN%"
goto :FAILED
:FAILED
>> "%AUTHKEYS_LOG%" echo Failed: %DATE% %TIME%
call :UI common.log_path "%AUTHKEYS_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 1

:CANCELLED
call :UI authkeys.cancelled
call :UI common.log_path "%AUTHKEYS_LOG%"
call :WAIT_BEFORE_CLOSE
exit /b 1

:HELPER_FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
if /I "%UI_LANGUAGE%"=="auto" set "UI_LANGUAGE=en"
call :UI authkeys.helper_failed
call :UI common.log_path "%AUTHKEYS_LOG%"
call :WAIT_BEFORE_CLOSE
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
call :UI common.directory_retry "%DELETE_TARGET%"
call :CHOICE 12
if errorlevel 2 exit /b 1
set /a DELETE_ATTEMPT=0
attrib.exe -R -S -H "%DELETE_TARGET%\*" /S /D >nul 2>&1
goto :DELETE_DIRECTORY_LOOP

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
