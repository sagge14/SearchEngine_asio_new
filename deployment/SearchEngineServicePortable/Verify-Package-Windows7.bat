@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "PACKAGE_ROOT=%~dp0"
set "CHECKSUM_FILE=%PACKAGE_ROOT%package-checksums.sha256"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

if not exist "%CHECKSUM_FILE%" (
    echo ERROR: package-checksums.sha256 is missing.
    exit /b 1
)
if not exist "%POWERSHELL%" (
    echo ERROR: Windows PowerShell is missing.
    exit /b 1
)

rem UTF-8 paths are read directly by PowerShell, including Cyrillic HTML names.
rem Hashing uses .NET so this check also works with Windows PowerShell 2.0.
"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $root=[IO.Path]::GetFullPath($env:PACKAGE_ROOT).TrimEnd([char]92)+[char]92; $count=0; $hash=[Security.Cryptography.SHA256]::Create(); try { foreach ($line in [IO.File]::ReadAllLines($env:CHECKSUM_FILE,[Text.Encoding]::UTF8)) { if ($line.Trim().Length -eq 0) { continue }; if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') { throw 'Invalid checksum record.' }; $expected=$Matches[1]; $relative=$Matches[2].Replace('/','\'); if ($relative.StartsWith('data\',[StringComparison]::OrdinalIgnoreCase)) { continue }; if ([IO.Path]::IsPathRooted($relative)) { throw 'Unsafe checksum path.' }; $file=[IO.Path]::GetFullPath([IO.Path]::Combine($root,$relative)); if (-not $file.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) { throw 'Checksum path escapes the package.' }; if (-not [IO.File]::Exists($file)) { throw ('Package file is missing: '+$relative) }; $stream=[IO.File]::OpenRead($file); try { $actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') } finally { $stream.Dispose() }; if (-not $actual.Equals($expected,[StringComparison]::OrdinalIgnoreCase)) { throw ('Package file is damaged: '+$relative) }; $count++ }; if ($count -eq 0) { throw 'No protected package files were listed.' } } finally { $hash.Dispose() }; if ([IO.Directory]::GetFiles($root,'searchclient-auth-token.json',[IO.SearchOption]::AllDirectories).Length -ne 0) { throw 'auth token must be stored in the current user LocalAppData, not in the portable package.' }; if ([IO.Directory]::GetFiles($root,'searchclient-auth-request.json',[IO.SearchOption]::AllDirectories).Length -ne 0) { throw 'unsigned auth request must not be stored in the portable package.' } } catch { Write-Host ('ERROR: '+$_.Exception.Message); exit 1 }; exit 0"
if errorlevel 1 exit /b 1

if /I not "%~1"=="/quiet" echo Package verification completed successfully.
exit /b 0
