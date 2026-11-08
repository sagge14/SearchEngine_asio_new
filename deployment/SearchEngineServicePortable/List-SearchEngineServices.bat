@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Read-only inventory of running SearchEngine services on this computer.
set "HELPER=%~dp0tools\SearchEngineConfig.exe"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
set "UI_LANGUAGE=auto"
set "SELECTION_FILE=%TEMP%\SE-ListServices-language-%RANDOM%-%RANDOM%.txt"
set "SE_LIST_OUTPUT=%TEMP%\SE-ListServices-output-%RANDOM%-%RANDOM%.txt"

if not exist "%HELPER%" (
    echo ERROR: SearchEngineConfig.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)
if not exist "%POWERSHELL%" (
    echo ERROR: Windows PowerShell is missing.
    pause
    exit /b 1
)

"%HELPER%" choose-language --output "%SELECTION_FILE%"
if errorlevel 1 goto :LANGUAGE_FAILED
for /f "usebackq tokens=1,* delims==" %%A in ("%SELECTION_FILE%") do if /I "%%A"=="language" set "UI_LANGUAGE=%%B"
del /Q "%SELECTION_FILE%" >nul 2>&1
if /I "%UI_LANGUAGE%"=="auto" goto :LANGUAGE_FAILED

"%HELPER%" list-running-services --language "%UI_LANGUAGE%" >"%SE_LIST_OUTPUT%" 2>&1
set "LIST_RC=%ERRORLEVEL%"
rem Decode the helper's UTF-8 output and print one labelled value per line.
"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $labels=$null; $settingsLabel='Settings file: '; if ($env:UI_LANGUAGE -eq 'ru') { $settingsLabel=(-join [char[]](1060,1072,1081,1083,32,1085,1072,1089,1090,1088,1086,1077,1082,58,32)) }; foreach ($line in [IO.File]::ReadAllLines($env:SE_LIST_OUTPUT,[Text.Encoding]::UTF8)) { if ($line.Contains([string][char]124)) { $parts=$line.Split([char]124); if ($parts.Length -eq 5) { if ($null -eq $labels) { $labels=$parts; continue }; Write-Host ''; for ($i=0; $i -lt 5; $i++) { Write-Host ($labels[$i].Trim()+': '+$parts[$i].Trim()) }; continue } }; if ($null -ne $labels -and $line -match '^  (?:[A-Za-z]:[\\/]|\\\\)') { Write-Host ($settingsLabel+$line.Trim()); continue }; Write-Host $line.TrimStart() }"
if errorlevel 1 (
    echo ERROR: Could not format the service list.
    set "LIST_RC=1"
)
del /Q "%SE_LIST_OUTPUT%" >nul 2>&1

echo.
"%HELPER%" script-message --language "%UI_LANGUAGE%" --id common.press_any_key
pause >nul
exit /b %LIST_RC%

:LANGUAGE_FAILED
del /Q "%SELECTION_FILE%" >nul 2>&1
echo ERROR: Could not select the interface language.
pause
exit /b 1
