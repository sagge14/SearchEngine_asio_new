@rem Shared installed-service resolver. Called inside the caller's SETLOCAL.
@rem Arguments: stop|start|restart, optional explicit instance id.
@rem Returns 0=resolved, 1=error, 2=cancelled. No service mutation here.
@echo off
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "SERVICE_INSTANCE=%~2"
set "SERVICE_NAME="
set "DATA_DIR="
set "SETTINGS_PATH="
set "ENDPOINT_PATH="
set "UI_LANGUAGE=en"
set "SELECTED_instance="
set "SELECTED_language="
set "RESOLVER_OUTPUT=%TEMP%\SE-Service-%RANDOM%-%RANDOM%.txt"
if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    exit /b 1
)
if not "%SERVICE_INSTANCE%"=="" goto :INSPECT
"%HELPER%" choose-installed-instance --purpose "%~1" --output "%RESOLVER_OUTPUT%"
set "PICKER_EXIT=%ERRORLEVEL%"
if not exist "%RESOLVER_OUTPUT%" goto :HELPER_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%RESOLVER_OUTPUT%") do set "SELECTED_%%A=%%B"
del /Q "%RESOLVER_OUTPUT%" >nul 2>&1
if defined SELECTED_language set "UI_LANGUAGE=%SELECTED_language%"
if "%PICKER_EXIT%"=="2" goto :CANCELLED
if "%PICKER_EXIT%"=="3" goto :NO_SERVICES
if not "%PICKER_EXIT%"=="0" goto :HELPER_FAILED
if not defined SELECTED_instance goto :HELPER_FAILED
if not defined SELECTED_language goto :HELPER_FAILED
set "SERVICE_INSTANCE=%SELECTED_instance%"

:INSPECT
rem Query SCM before opening Settings.json; never fall back to another instance.
rem Helper output is UTF-8. Keep the picker in the original console code page.
set "RESOLVER_CODEPAGE="
for /f "tokens=2 delims=:" %%C in ('chcp') do set "RESOLVER_CODEPAGE=%%C"
if not defined RESOLVER_CODEPAGE goto :HELPER_FAILED
set "RESOLVER_CODEPAGE=%RESOLVER_CODEPAGE: =%"
chcp 65001 >nul
"%HELPER%" inspect-installed --instance "%SERVICE_INSTANCE%" > "%RESOLVER_OUTPUT%"
set "INSPECT_EXIT=%ERRORLEVEL%"
if not "%INSPECT_EXIT%"=="0" goto :INSPECT_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%RESOLVER_OUTPUT%") do (
    if /I "%%A"=="instance" set "SERVICE_INSTANCE=%%B"
    if /I "%%A"=="service_name" set "SERVICE_NAME=%%B"
    if /I "%%A"=="data_dir" set "DATA_DIR=%%B"
    if /I "%%A"=="settings_path" set "SETTINGS_PATH=%%B"
    if /I "%%A"=="endpoint_path" set "ENDPOINT_PATH=%%B"
)
chcp %RESOLVER_CODEPAGE% >nul
del /Q "%RESOLVER_OUTPUT%" >nul 2>&1
if not defined SERVICE_NAME goto :HELPER_FAILED
if not defined DATA_DIR goto :HELPER_FAILED
if not defined SETTINGS_PATH goto :HELPER_FAILED
if not defined ENDPOINT_PATH goto :HELPER_FAILED
exit /b 0

:INSPECT_FAILED
chcp %RESOLVER_CODEPAGE% >nul
goto :HELPER_FAILED
:NO_SERVICES
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id control.no_services
exit /b 1
:CANCELLED
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id control.cancelled
exit /b 2
:HELPER_FAILED
del /Q "%RESOLVER_OUTPUT%" >nul 2>&1
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id control.resolve_failed
exit /b 1
