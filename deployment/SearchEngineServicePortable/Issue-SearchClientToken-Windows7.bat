@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ISSUER=%~dp0tools\SearchClientTokenIssuer.exe"
if not exist "%ISSUER%" (
    echo ERROR: SearchClientTokenIssuer.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

rem The Unicode executable owns language selection, messages and the final pause.
rem /quiet skips the language prompt and pause for scripted use.
"%ISSUER%" --console-wrapper %*
exit /b %ERRORLEVEL%
