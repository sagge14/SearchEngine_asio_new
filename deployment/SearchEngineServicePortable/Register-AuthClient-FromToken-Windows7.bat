@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "AUTH_TOOL=%PACKAGE_ROOT%tools\AuthDbTool.exe"
set "CONFIG_TOOL=%PACKAGE_ROOT%tools\SearchEngineConfig.exe"
set "SCRIPT=%PACKAGE_ROOT%tools\Register-AuthClientFromToken.ps1"

if not exist "%AUTH_TOOL%" (
    echo ERROR: AuthDbTool.exe is missing. Use the complete portable folder.
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

echo Register auth client from USB token into auth_clients.sqlite
echo Architecture: %TARGET_ARCH%
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -AuthDbToolPath "%AUTH_TOOL%" -SearchEngineConfigPath "%CONFIG_TOOL%" %*
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

:WAIT_BEFORE_CLOSE
if /I "%~1"=="/quiet" exit /b 0
pause
exit /b 0
