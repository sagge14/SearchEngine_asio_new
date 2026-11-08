@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PACKAGE_ROOT=%~dp0"
cd /d "%PACKAGE_ROOT%" || exit /b 1
"%~dp0app\ZagEditor.exe" --config "%PACKAGE_ROOT%data\zag_editor.ini" %*
exit /b %ERRORLEVEL%
