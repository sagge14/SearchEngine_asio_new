@echo off
setlocal EnableExtensions DisableDelayedExpansion

call "%~dp0Install-SearchEngineService.bat" /LocalMachine
set "INSTALL_EXIT=%ERRORLEVEL%"

if exist "%~dp0tools\SearchEngineConfig.exe" (
    "%~dp0tools\SearchEngineConfig.exe" script-message --language ru --id common.press_any_key
)
pause >nul
exit /b %INSTALL_EXIT%
