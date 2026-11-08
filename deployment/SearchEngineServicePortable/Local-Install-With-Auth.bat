@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "LOCAL_FAST_WRAPPER=%~f0"
fsutil.exe dirty query %SystemDrive% >nul 2>&1
if not errorlevel 1 goto :INSTALL
if /I "%~1"=="/Elevated" exit /b 1
REM Windows displays its normal UAC consent; no installer choices are needed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { Start-Process -FilePath $env:ComSpec -ArgumentList ('/d /s /c ""'+$env:LOCAL_FAST_WRAPPER+'" /Elevated"') -Verb RunAs -WindowStyle Normal } catch { Write-Error $_; exit 1 }"
exit /b %ERRORLEVEL%
:INSTALL
call "%~dp0Install-SearchEngineService.bat" /LocalMachine /AuthRequired
set "INSTALL_EXIT=%ERRORLEVEL%"
echo.
echo Installer exit code: %INSTALL_EXIT%
echo Press any key to close this window...
pause >nul
exit /b %INSTALL_EXIT%

