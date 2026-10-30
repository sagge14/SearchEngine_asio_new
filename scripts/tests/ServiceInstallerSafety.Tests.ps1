# Runs real BAT files in disposable directories with fake executables only.
# No Windows services, registry keys, firewall rules, or real data are changed.
[CmdletBinding()]
param(
    [string]$InstallerPath,
    [string]$WrapperPath,
    [string]$FixtureExecutablePath,
    [string]$ReportPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$templates = Join-Path $projectRoot 'deployment\SearchEngineServicePortable'
if (-not $InstallerPath) { $InstallerPath = Join-Path $templates 'Install-SearchEngineService-Windows7.bat' }
if (-not $WrapperPath) { $WrapperPath = Join-Path $templates 'Local-Install-With-Auth.bat' }
$installer = [IO.File]::ReadAllText($InstallerPath).Replace('{{ARCHITECTURE}}', 'x86').Replace('{{VC_REDIST_FILE}}', 'vc_redist.x86.exe')
$wrapper = [IO.File]::ReadAllText($WrapperPath)
$root = Join-Path ([IO.Path]::GetTempPath()) ('SE-InstallerSafety-' + [Guid]::NewGuid().ToString('N'))
$results = New-Object Collections.Generic.List[object]
$ascii = [Text.Encoding]::ASCII
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $fake = Join-Path $root 'fake.exe'
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if ($FixtureExecutablePath -and (Test-Path -LiteralPath $FixtureExecutablePath -PathType Leaf)) {
        Copy-Item -LiteralPath $FixtureExecutablePath -Destination $fake
    } else {
        & $compiler /nologo /target:exe "/out:$fake" (Join-Path $projectRoot 'tests\config\FakeInstallerConfig.cs')
        if ($LASTEXITCODE -ne 0) { throw 'Installer fixture compilation failed' }
        if ($FixtureExecutablePath) { Copy-Item -LiteralPath $fake -Destination $FixtureExecutablePath }
    }

    function Run-Case([string]$Mode, [bool]$SeparateCaller) {
        $case = Join-Path $root ($Mode + '-' + $SeparateCaller)
        $package = Join-Path $case 'package with spaces'
        $caller = if ($SeparateCaller) { Join-Path $case 'caller with spaces' } else { $package }
        $temp = Join-Path $case 'temp'
        $stubs = Join-Path $case 'stubs'
        foreach ($path in @($package, $caller, $temp, $stubs, (Join-Path $package 'app'),
                (Join-Path $package 'tools'), (Join-Path $package 'data'), (Join-Path $package 'prerequisites'))) {
            New-Item -ItemType Directory -Path $path -Force | Out-Null
        }
        [IO.File]::WriteAllText((Join-Path $package 'Install-SearchEngineService.bat'), $installer, $ascii)
        [IO.File]::WriteAllText((Join-Path $package 'Local-Install-With-Auth.bat'), $wrapper, $ascii)
        foreach ($fastName in @('Local-Install-Without-Auth.bat')) {
            Copy-Item -LiteralPath (Join-Path $templates $fastName) -Destination (Join-Path $package $fastName)
        }
        foreach ($name in @('SearchEngine', 'SearchEngineConfig', 'SearchClientTokenIssuer', 'AuthDbTool')) {
            $subdir = if ($name -eq 'SearchEngine') { 'app' } else { 'tools' }
            if (($Mode -eq 'missing-app' -and $name -eq 'SearchEngine') -or
                ($Mode -eq 'missing-helper' -and $name -eq 'SearchEngineConfig')) { continue }
            Copy-Item -LiteralPath $fake -Destination (Join-Path $package ($subdir + '\' + $name + '.exe'))
        }
        foreach ($name in @('fsutil', 'sc', 'reg', 'netsh')) {
            Copy-Item -LiteralPath $fake -Destination (Join-Path $stubs ($name + '.exe'))
        }
        foreach ($relative in @('data\Settings.json', 'data\OEM866.INI', 'README.txt', 'INSTALLATION_GUIDE_RU.txt',
                'tools\vcruntime140.dll', 'tools\vcruntime140_1.dll', 'tools\msvcp140.dll',
                'prerequisites\vc_redist.x86.exe', 'prerequisites\vc_redist.x64.exe')) {
            if ($Mode -eq 'missing-prerequisite' -and $relative.StartsWith('prerequisites')) { continue }
            [IO.File]::WriteAllText((Join-Path $package $relative), 'fixture', $ascii)
        }
        [IO.File]::WriteAllText((Join-Path $package 'ServiceInstance.cmd'), "@set `"SERVICE_INSTANCE=default`"`r`n", $ascii)
        if ($Mode -ne 'missing-verifier') {
            $verifyExit = if ($Mode -eq 'damaged-package') { 1 } else { 0 }
            [IO.File]::WriteAllText((Join-Path $package 'Verify-Package.bat'), "@exit /b $verifyExit`r`n", $ascii)
        }
        $sentinel = Join-Path $caller 'keep-me.txt'
        $inherited = Join-Path $temp 'inherited-file.txt'
        [IO.File]::WriteAllText($sentinel, 'caller contents must survive', $ascii)
        [IO.File]::WriteAllText($inherited, 'inherited temp paths are not owned', $ascii)
        $before = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
            [pscustomobject]@{ Path = $_.FullName; Hash = (Get-FileHash -LiteralPath $_.FullName).Hash }
        })
        $log = Join-Path $case 'calls.log'
        [IO.File]::WriteAllText($log, '')
        # Failure cases call the shared installer directly, so a simulated
        # non-admin machine cannot trigger the wrappers' real UAC elevation.
        $entry = 'Install-SearchEngineService.bat'
        $arguments = ' /LocalMachine /AuthRequired'
        if ($Mode -eq 'success-fast-auth') { $entry = 'Local-Install-With-Auth.bat'; $arguments = '' }
        if ($Mode -eq 'success-fast-none') { $entry = 'Local-Install-Without-Auth.bat'; $arguments = '' }
        if ($Mode -eq 'unknown-argument') { $entry = 'Install-SearchEngineService.bat'; $arguments = ' /invalid' }
        if ($Mode -eq 'argument-conflict') { $entry = 'Install-SearchEngineService.bat'; $arguments = ' /LocalMachine /validate' }
        if ($Mode -eq 'validate') { $entry = 'Install-SearchEngineService.bat'; $arguments = ' /validate' }
        # Skip real Runtime installation even if the fixture's probe fails.
        if ($Mode -eq 'crt-load-failure') { $entry = 'Install-SearchEngineService.bat'; $arguments = ' /LocalMachine /SkipVcRedist' }
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = $env:COMSPEC
        $info.Arguments = '/d /s /c ""' + (Join-Path $package $entry) + '"' + $arguments + '"'
        $info.WorkingDirectory = $caller
        $info.UseShellExecute = $false
        $info.RedirectStandardInput = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.EnvironmentVariables['PATH'] = $stubs + ';' + $env:PATH
        $info.EnvironmentVariables['TEMP'] = $temp
        $info.EnvironmentVariables['TMP'] = $temp
        foreach ($name in @('ProgramData', 'ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432', 'LOCALAPPDATA')) {
            $info.EnvironmentVariables[$name] = Join-Path $case $name
        }
        foreach ($name in @('INSTANCE_TEMP', 'CONFIG_TEMP', 'ENDPOINT_TEMP', 'HELPER_OUTPUT')) {
            if ($SeparateCaller) { $info.EnvironmentVariables[$name] = $inherited }
            else { $info.EnvironmentVariables.Remove($name) }
        }
        $info.EnvironmentVariables['SE_INSTALL_TEST_MODE'] = $Mode
        $info.EnvironmentVariables['SE_INSTALL_TEST_LOG'] = $log
        $info.EnvironmentVariables['SE_INSTALL_TEST_STARTED'] = Join-Path $case 'started'
        $info.EnvironmentVariables['SE_INSTALL_TEST_INFO_COUNT'] = Join-Path $case 'info-count'
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $info
        try {
            [void]$process.Start()
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $process.StandardInput.Close()
            if (-not $process.WaitForExit(15000)) { $process.Kill(); throw "Fixture timed out: $Mode" }
            $output = $stdout.Result + $stderr.Result
            $expectedExit = if ($Mode -in @('validate', 'success', 'success-none', 'success-fast-auth', 'success-fast-none')) { 0 } else { 1 }
            $calls = [IO.File]::ReadAllText($log)
            $failures = New-Object Collections.Generic.List[string]
            if ($process.ExitCode -ne $expectedExit) { $failures.Add("Expected exit $expectedExit, got $($process.ExitCode)") }
            foreach ($file in $before) {
                if (-not [IO.File]::Exists($file.Path) -or (Get-FileHash -LiteralPath $file.Path).Hash -ne $file.Hash) {
                    $failures.Add('Package file changed: ' + $file.Path)
                }
            }
            if (-not [IO.File]::Exists($sentinel)) { $failures.Add('Caller file deleted') }
            if (-not [IO.File]::Exists($inherited)) { $failures.Add('Inherited temp file deleted') }
            if (@(Get-ChildItem -LiteralPath $temp -File | Where-Object Name -ne 'inherited-file.txt').Count -ne 0) {
                $failures.Add('Installer temp files leaked')
            }
            if ($Mode -in @('success', 'success-none', 'success-fast-auth', 'success-fast-none')) {
                if (-not $calls.Contains("sc`tcreate`tSearchEngineService-2026")) { $failures.Add('Fake service was not created') }
                if (-not $calls.Contains("SearchEngine`t--initial-update")) { $failures.Add('Fake initial update was not run') }
                if (-not $calls.Contains("health`t--port`t25027")) { $failures.Add('Fake service health was not checked') }
                if ($Mode -in @('success-none','success-fast-none') -and
                    ($calls.Contains("SearchClientTokenIssuer`t") -or $calls.Contains("AuthDbTool`t"))) {
                    $failures.Add('Anonymous installation generated keys or registered a token')
                }
                if ($Mode -in @('success','success-fast-auth') -and -not $calls.Contains("SearchClientTokenIssuer`t")) {
                    $failures.Add('Required installation did not provision the local token')
                }
                if ($Mode -like 'success-fast-*') {
                    $expectedMode = if ($Mode -eq 'success-fast-none') { 'none' } else { 'required' }
                    if (-not $calls.Contains("--authentication-mode`t$expectedMode") -or $calls.Contains('--choose-authentication')) {
                        $failures.Add('Fast installation did not select its mode without a prompt')
                    }
                }
            } elseif ($calls.Contains("sc`t") -or $calls.Contains("reg`t") -or $calls.Contains("netsh`t")) {
                $failures.Add('Preflight failure reached service or OS changes')
            }
            if ($entry -like 'Local-Install-*.bat' -and -not $output.Contains('Installer exit code: ' + $expectedExit)) {
                $failures.Add('Wrapper lost final exit code / message')
            }
            if ($Mode -in @('missing-app','missing-helper','missing-prerequisite','missing-verifier','damaged-package','not-admin','crt-load-failure')) {
                if ($calls.Contains("script-message`t")) { $failures.Add('Helper launched before Runtime was ready') }
            }
            $result = [pscustomobject]@{ Mode=$Mode; SeparateCaller=$SeparateCaller; ExitCode=$process.ExitCode;
                Passed=($failures.Count -eq 0); Failures=@($failures.ToArray()); Output=$output; Calls=$calls }
            $results.Add($result)
            Write-Host (('{0}: {1}, separate caller={2}' -f $(if ($result.Passed) { 'PASS' } else { 'FAIL' }), $Mode, $SeparateCaller))
        } finally { $process.Dispose() }
    }
    foreach ($mode in @('missing-app','missing-helper','missing-prerequisite','missing-verifier','damaged-package',
            'not-admin','crt-load-failure','instance-helper-failure','missing-year','template-inspect-failure',
            'unknown-argument','argument-conflict','validate','success','success-none','success-fast-auth','success-fast-none')) {
        Run-Case $mode $false
        Run-Case $mode $true
    }
    if ($ReportPath) { $results.ToArray() | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
    $failed = @($results | Where-Object { -not $_.Passed })
    if ($failed.Count -gt 0) { throw ($failed | ConvertTo-Json -Depth 5) }
    Write-Host "Installer safety tests passed: $($results.Count) cases. No real service was touched."
} finally {
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($root)) -ne $tempParent -or
            -not [IO.Path]::GetFileName($root).StartsWith('SE-InstallerSafety-')) { throw 'Unsafe fixture cleanup path' }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
