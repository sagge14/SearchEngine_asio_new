# Executes the real batch templates against fake EXEs. No service operations.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Join-Path ([IO.Path]::GetTempPath()) ('SE-ServiceControl-tests-' + [Guid]::NewGuid().ToString('N'))
$ascii = [Text.Encoding]::ASCII
$passed = 0
function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:passed++
}
try {
    New-Item -ItemType Directory -Path (Join-Path $root 'tools') -Force | Out-Null
    $data = Join-Path $root ('archive path ' + [char]0x0414 + [char]0x0430 + [char]0x043D + [char]0x043D + [char]0x044B + [char]0x0435 + '! & (2026)')
    New-Item -ItemType Directory -Path $data | Out-Null
    [IO.File]::WriteAllText((Join-Path $data 'client-endpoint.txt'), "port=25027`r`n", $ascii)
    $fake = Join-Path $root 'tools\SearchEngineConfig.exe'
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe "/out:$fake" (Join-Path $projectRoot 'tests\config\FakeServiceConfig.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Fake helper compilation failed' }
    foreach ($name in @('fsutil','netsh')) { Copy-Item -LiteralPath $fake -Destination (Join-Path $root ($name+'.exe')) }
    $templates = Join-Path $projectRoot 'deployment\SearchEngineServicePortable'
    foreach ($name in @('Stop','Start','Restart')) {
        Copy-Item -LiteralPath (Join-Path $templates ($name+'-SearchEngineService-Windows7.bat')) -Destination (Join-Path $root ($name+'.bat'))
    }
    Copy-Item -LiteralPath (Join-Path $templates 'Resolve-ServiceInstance.cmd') -Destination $root
    # A stale default in the portable package must not override the real choice.
    [IO.File]::WriteAllText((Join-Path $root 'ServiceInstance.cmd'), "set `"SERVICE_INSTANCE=default`"`r`n", $ascii)

    function Run-Case([string]$Action, [string]$Mode, [string]$Instance = '', [string]$Selected = '2026') {
        $log = Join-Path $root 'calls.log'
        [IO.File]::WriteAllText($log, '')
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = $env:COMSPEC
        $info.Arguments = '/d /s /c ""' + (Join-Path $root ($Action+'.bat')) + '" ' + $Instance + '"'
        $info.WorkingDirectory = $root
        $info.UseShellExecute = $false
        $info.RedirectStandardInput = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.EnvironmentVariables['PATH'] = $root+';'+$env:PATH
        $info.EnvironmentVariables['SE_TEST_LOG'] = $log
        $info.EnvironmentVariables['SE_TEST_DATA'] = $data
        $info.EnvironmentVariables['SE_TEST_SELECTED'] = $Selected
        $info.EnvironmentVariables['SE_TEST_MODE'] = $Mode
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $info
        try {
            [void]$process.Start()
            $outputTask = $process.StandardOutput.ReadToEndAsync()
            $errorTask = $process.StandardError.ReadToEndAsync()
            $process.StandardInput.WriteLine('x')
            $process.StandardInput.Close()
            if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Batch fixture timed out' }
            $result = [pscustomobject]@{
                ExitCode=$process.ExitCode
                Calls=[IO.File]::ReadAllText($log)
                Output=$outputTask.Result + $errorTask.Result
            }
        } finally { $process.Dispose() }
        return $result
    }
    foreach ($action in @('Stop','Start','Restart')) {
        $result = Run-Case $action 'ok'
        Assert-That ($result.ExitCode -eq 0) "$action picker failed: $($result.Output)"
        Assert-That ($result.Calls.Contains("choose-installed-instance`t--purpose`t"+$action.ToLowerInvariant())) "$action has wrong picker purpose"
        Assert-That ($result.Calls.Contains("inspect-installed`t--instance`t2026")) "$action did not resolve selected 2026"
        Assert-That (-not $result.Calls.Contains("--instance`tdefault")) "$action used stale default"
        Assert-That ($result.Calls.Contains("service-control`t--action`t")) "$action did not control chosen service"
        if ($action -ne 'Stop') {
            Assert-That ($result.Calls.Contains("inspect`t--settings`t"+(Join-Path $data 'Settings.json'))) "$action lost Unicode, spaces, !, &, or parentheses in data-dir"
            Assert-That ($result.Calls.Contains("health`t--port`t25027")) "$action did not check installed port"
        }
        $result = Run-Case $action 'ok' '2027'
        Assert-That ($result.ExitCode -eq 0 -and $result.Calls.Contains("--instance`t2027")) "$action explicit 2027 failed"
        Assert-That (-not $result.Calls.Contains('choose-installed-instance')) "$action explicit argument opened picker"
        $result = Run-Case $action 'cancel'
        Assert-That ($result.ExitCode -eq 0 -and -not $result.Calls.Contains('service-control')) "$action cancellation mutated service"
        foreach ($mode in @('empty','picker-error','access-denied','not-admin')) {
            $result = Run-Case $action $mode
            Assert-That ($result.ExitCode -ne 0 -and -not $result.Calls.Contains('service-control')) "$action $mode did not fail safely"
        }
        $result = Run-Case $action 'ok' 'missing'
        Assert-That ($result.ExitCode -ne 0 -and -not $result.Calls.Contains('choose-installed-instance') -and -not $result.Calls.Contains('service-control')) "$action missing explicit instance silently switched target"
        $result = Run-Case $action 'ok' '' '2027'
        Assert-That ($result.ExitCode -eq 0 -and $result.Calls.Contains("--instance`t2027")) "$action cannot select second installed instance"
    }
    $result = Run-Case 'Stop' 'invalid-settings'
    Assert-That ($result.ExitCode -eq 0 -and -not $result.Calls.Contains("inspect`t--settings")) 'Stop requires Settings.json'
    foreach ($action in @('Start','Restart')) {
        $result = Run-Case $action 'invalid-settings'
        Assert-That ($result.ExitCode -ne 0 -and -not $result.Calls.Contains('service-control')) "$action operated with invalid settings"
        $result = Run-Case $action 'health-error'
        Assert-That ($result.ExitCode -ne 0) "$action ignored health failure"
    }
    $result = Run-Case 'Restart' 'stop-error'
    Assert-That ($result.ExitCode -ne 0 -and -not $result.Calls.Contains("service-control`t--action`tstart")) 'Restart starts after failed stop'
    $result = Run-Case 'Start' 'start-error'
    Assert-That ($result.ExitCode -ne 0 -and -not $result.Calls.Contains("health`t")) 'Start ignores native start failure'
    $result = Run-Case 'Stop' 'ok' 'DEFAULT'
    Assert-That ($result.ExitCode -eq 0 -and $result.Calls.Contains("service-control`t--action`tstop`t--instance`tdefault")) 'Legacy default case is inconsistent'
    Write-Host "Service control batch tests passed: $passed assertions. No real service was touched."
} finally {
    # root is a checked, uniquely-created test directory directly beneath TEMP.
    if ([IO.Path]::GetDirectoryName($root) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unsafe fixture cleanup path' }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
