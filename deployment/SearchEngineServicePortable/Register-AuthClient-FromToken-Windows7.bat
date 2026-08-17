@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "AUTH_TOOL=%PACKAGE_ROOT%tools\AuthDbTool.exe"
set "CONFIG_TOOL=%PACKAGE_ROOT%tools\SearchEngineConfig.exe"
set "SCRIPT=%PACKAGE_ROOT%tools\Register-AuthClientFromToken.ps1"
set "INSTANCE_TEMP=%TEMP%\SearchEngine-AuthRegister-%RANDOM%-%RANDOM%.txt"
set "SERVICE_INSTANCE="
set "TOKEN_PATH="

if not exist "%AUTH_TOOL%" (
    echo ERROR: AuthDbTool.exe is missing. Use the complete portable folder.
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
if not exist "%CONFIG_TOOL%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
if not exist "%SCRIPT%" (
    echo ERROR: Register-AuthClientFromToken.ps1 is missing. Use the complete portable folder.
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

where.exe powershell.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: powershell.exe is not available.
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

echo Register auth client from auth token into auth_clients.sqlite
echo Architecture: %TARGET_ARCH%
echo.

REM Parse optional args: instance id and/or token path.
:PARSE_ARGS
if "%~1"=="" goto :ARGS_DONE
set "ARG=%~1"
if /I "%ARG%"=="/token" (
    if "%~2"=="" (
        echo ERROR: /token requires a file path.
        call :WAIT_BEFORE_CLOSE
        exit /b 1
    )
    set "TOKEN_PATH=%~2"
    shift
    shift
    goto :PARSE_ARGS
)
if "%ARG:~0,1%"=="/" (
    echo ERROR: Unknown argument %~1
    echo Supported: [instance-id] [/token path]
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
if not "%SERVICE_INSTANCE%"=="" (
    echo ERROR: Unexpected argument %~1
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)
set "SERVICE_INSTANCE=%~1"
shift
goto :PARSE_ARGS

:ARGS_DONE
if not "%SERVICE_INSTANCE%"=="" goto :INSTANCE_READY

REM Interactive selection must run from cmd.exe, not from inside PowerShell:
REM otherwise SearchEngineConfig prompts are easy to miss / look like a hang.
echo Selecting installed SearchEngine service...
"%CONFIG_TOOL%" choose-installed-instance --purpose register-auth --output "%INSTANCE_TEMP%"
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

echo Instance: %SERVICE_INSTANCE%
echo.

REM -STA: OpenFileDialog needs STA on PowerShell 2.0 (Windows 7).
REM The .ps1 calls "exit 1" on failure so PS 2.0 -File returns non-zero.
if "%TOKEN_PATH%"=="" (
    powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%SCRIPT%" -AuthDbToolPath "%AUTH_TOOL%" -SearchEngineConfigPath "%CONFIG_TOOL%" -InstanceId "%SERVICE_INSTANCE%"
) else (
    powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%SCRIPT%" -AuthDbToolPath "%AUTH_TOOL%" -SearchEngineConfigPath "%CONFIG_TOOL%" -InstanceId "%SERVICE_INSTANCE%" -TokenPath "%TOKEN_PATH%"
)
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" (
    echo.
    echo ERROR: Registration failed with exit code %EXIT_CODE%.
    call :WAIT_BEFORE_CLOSE
    exit /b %EXIT_CODE%
)

echo.
echo Registration completed.
call :WAIT_BEFORE_CLOSE
exit /b 0

:NO_INSTALLED_SERVICES
del /Q "%INSTANCE_TEMP%" >nul 2>&1
echo ERROR: No installed SearchEngine services were found.
call :WAIT_BEFORE_CLOSE
exit /b 3

:CANCELLED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
echo Registration cancelled.
call :WAIT_BEFORE_CLOSE
exit /b 2

:HELPER_FAILED
del /Q "%INSTANCE_TEMP%" >nul 2>&1
echo ERROR: SearchEngineConfig could not list installed services.
call :WAIT_BEFORE_CLOSE
exit /b 1

:INVALID_INSTANCE
echo ERROR: Invalid service instance id "%SERVICE_INSTANCE%".
echo Use 1-32 ASCII letters, digits, underscore or hyphen; the first character must be alphanumeric.
call :WAIT_BEFORE_CLOSE
exit /b 1

:WAIT_BEFORE_CLOSE
if /I "%~1"=="/quiet" exit /b 0
pause
exit /b 0
