@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "CHECKSUM_FILE=%PACKAGE_ROOT%package-checksums.sha256"

if not exist "%CHECKSUM_FILE%" (
    echo ERROR: package-checksums.sha256 is missing.
    exit /b 1
)

where.exe certutil.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: certutil.exe is not available.
    exit /b 1
)

set "VERIFY_FAILED="
for /f "usebackq tokens=1,*" %%H in ("%CHECKSUM_FILE%") do call :VERIFY_FILE "%%I" "%%H"
if defined VERIFY_FAILED exit /b 1

if /I not "%~1"=="/quiet" echo Package verification completed successfully.
exit /b 0

:VERIFY_FILE
set "RELATIVE_FILE=%~1"
set "EXPECTED_HASH=%~2"
set "ACTUAL_HASH="

rem Package data is a mutable template (Settings.json, OEM866.INI, ignore.txt).
rem Older checksum files may still list those paths; skip the whole data\ tree.
set "RELATIVE_FILE=%RELATIVE_FILE:/=\%"
set "RELATIVE_PREFIX=%RELATIVE_FILE:~0,5%"
if /I "%RELATIVE_PREFIX%"=="data\" exit /b 0

if not exist "%PACKAGE_ROOT%%RELATIVE_FILE%" (
    echo ERROR: Package file is missing: %RELATIVE_FILE%
    set "VERIFY_FAILED=1"
    exit /b 1
)

for /f "delims=" %%A in ('certutil.exe -hashfile "%PACKAGE_ROOT%%RELATIVE_FILE%" SHA256 2^>nul ^| findstr.exe /V ":"') do set "ACTUAL_HASH=%%A"
set "ACTUAL_HASH=%ACTUAL_HASH: =%"
if /I not "%ACTUAL_HASH%"=="%EXPECTED_HASH%" (
    echo ERROR: Package file is damaged: %RELATIVE_FILE%
    set "VERIFY_FAILED=1"
    exit /b 1
)
exit /b 0
