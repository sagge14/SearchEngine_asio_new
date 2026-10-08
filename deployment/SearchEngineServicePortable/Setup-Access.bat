@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "WIZARD=%~dp0tools\SearchEngineAccessSetup.exe"
if not exist "%WIZARD%" (
    echo ERROR: SearchEngineAccessSetup.exe is missing. Use the complete portable package.
    pause
    exit /b 1
)
"%WIZARD%"
exit /b %ERRORLEVEL%
