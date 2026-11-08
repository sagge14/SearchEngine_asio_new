@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_ROOT=%~dp0"
cd /d "%PACKAGE_ROOT%" || exit /b 1
"%~dp0app\BackupRestore.exe" %*
exit /b %ERRORLEVEL%
