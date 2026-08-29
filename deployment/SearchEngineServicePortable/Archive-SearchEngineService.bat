@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ARCHIVE_TOOL=%~dp0tools\SearchEngineArchive.exe"
if not exist "%ARCHIVE_TOOL%" (
    echo ERROR: SearchEngineArchive.exe is missing. Use the complete portable folder.
    pause
    exit /b 1
)

fsutil.exe dirty query %SystemDrive% >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run Archive-SearchEngineService.bat as Administrator.
    pause
    exit /b 1
)

"%ARCHIVE_TOOL%" %*
set "ARCHIVE_RC=%ERRORLEVEL%"
echo.
if "%ARCHIVE_RC%"=="0" (
    echo SearchEngineArchive completed successfully.
) else (
    echo SearchEngineArchive failed or was cancelled. Exit code: %ARCHIVE_RC%
)
pause
exit /b %ARCHIVE_RC%
