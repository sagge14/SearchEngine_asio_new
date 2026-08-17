@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "TARGET_ARCH={{ARCHITECTURE}}"
set "ISSUER=%PACKAGE_ROOT%tools\SearchClientTokenIssuer.exe"

if not exist "%ISSUER%" (
    echo ERROR: SearchClientTokenIssuer.exe is missing. Use the complete portable folder.
    call :WAIT_BEFORE_CLOSE
    exit /b 1
)

echo Issue SearchClient auth token (USB or computer)
echo Architecture: %TARGET_ARCH%
echo.
echo Interactive: choose 1 USB or 2 Computer.
echo For a computer token, default path is %ProgramData%\SearchEngine\searchclient-auth-token.json
echo Press Enter at "output path" to open a Save dialog in that folder.
echo Extra arguments are passed to SearchClientTokenIssuer.exe.
echo.

"%ISSUER%" %*
set "EXIT_CODE=%ERRORLEVEL%"
if "%EXIT_CODE%"=="0" (
    echo.
    echo Token issue completed.
    call :WAIT_BEFORE_CLOSE
    exit /b 0
)
if "%EXIT_CODE%"=="2" (
    echo.
    echo Token issue cancelled.
    call :WAIT_BEFORE_CLOSE
    exit /b 2
)

echo.
echo ERROR: Token issuer failed with exit code %EXIT_CODE%.
call :WAIT_BEFORE_CLOSE
exit /b %EXIT_CODE%

:WAIT_BEFORE_CLOSE
if /I "%~1"=="/quiet" exit /b 0
pause
exit /b 0
