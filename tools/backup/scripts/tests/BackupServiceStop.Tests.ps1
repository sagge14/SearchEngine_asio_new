# Safe unit tests for BackupService stop helpers (mock service state only).
# Does not install, stop, or kill a real Windows service.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$common = Join-Path (Split-Path $PSScriptRoot -Parent) 'BackupServiceStop.Common.ps1'
. $common

$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        $script:passed++
        Write-Host "PASS: $Message"
    } else {
        $script:failures.Add($Message) | Out-Null
        Write-Host "FAIL: $Message"
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($Expected -eq $Actual) {
        $script:passed++
        Write-Host "PASS: $Message"
    } else {
        $msg = "$Message (expected='$Expected' actual='$Actual')"
        $script:failures.Add($msg) | Out-Null
        Write-Host "FAIL: $msg"
    }
}

function New-FakeRuntime {
    param(
        [bool]$Exists = $true,
        [string]$State = 'Running',
        [int]$ProcessId = 4242,
        [string]$ServiceType = 'Own Process'
    )
    return [pscustomobject]@{
        Exists = $Exists
        State = $State
        ProcessId = $ProcessId
        ServiceType = $ServiceType
        PathName = '"C:\Test\BackupService.exe" --service'
    }
}

Write-Host '=== BackupServiceStop mock tests ==='

# 1. Enter selects Graceful
$r1 = Resolve-BackupServiceStopMode -CanPrompt $true -PromptReader { '' } `
    -ServiceName 'SearchEngineBackupService' -InstanceId 'default'
Assert-Equal 'Graceful' $r1.Mode '1. Enter selects Graceful'
Assert-True (-not $r1.Cancelled) '1. Enter is not cancel'

# 2. Explicit Graceful does not force-kill
$forcedCalls = New-Object System.Collections.Generic.List[int]
$state = @{ Current = New-FakeRuntime }
$resultGraceful = Invoke-BackupServiceStop -ServiceName 'SearchEngineBackupService' `
    -InstanceId 'default' -StopMode Graceful -TimeoutSeconds 5 -ImmediateGraceSeconds 0 `
    -GetRuntimeInfo {
        param([string]$ServiceName)
        return $state.Current
    } `
    -RequestStop {
        param([string]$Name, [string]$Mode)
        Assert-Equal 'Graceful' $Mode '2. RequestStop mode is Graceful'
        $state.Current = New-FakeRuntime -State Stopped -ProcessId 0
    } `
    -WaitForStopped { param($n, $t) return $true } `
    -ForceStopProcess { param([int]$ProcessId) $forcedCalls.Add($ProcessId) | Out-Null } `
    -Sleep { param([int]$Seconds) }
Assert-Equal 'Stopped' $resultGraceful.Status '2. Graceful reaches Stopped'
Assert-True (-not $resultGraceful.Forced) '2. Graceful Forced=false'
Assert-Equal 0 $forcedCalls.Count '2. Explicit Graceful does not call ForceStopProcess'

# 3. Immediate kills only the re-verified PID
$forcedCalls.Clear()
$state.Current = New-FakeRuntime -ProcessId 7777
$killLog = New-Object System.Collections.Generic.List[int]
$resultImmediate = Invoke-BackupServiceStop -ServiceName 'SearchEngineBackupService' `
    -InstanceId 'default' -StopMode Immediate -TimeoutSeconds 5 -ImmediateGraceSeconds 0 `
    -CurrentProcessId 111 `
    -GetRuntimeInfo {
        param([string]$ServiceName)
        return $state.Current
    } `
    -RequestStop {
        param([string]$Name, [string]$Mode)
        Assert-Equal 'Immediate' $Mode '3. RequestStop mode is Immediate'
        # Still running after STOP request.
        $state.Current = New-FakeRuntime -ProcessId 7777
    } `
    -WaitForStopped {
        param($n, $t)
        $state.Current = New-FakeRuntime -State Stopped -ProcessId 0
        return $true
    } `
    -ForceStopProcess {
        param([int]$ProcessId)
        $killLog.Add($ProcessId) | Out-Null
        $state.Current = New-FakeRuntime -State Stopped -ProcessId 0
    } `
    -TestProcessExists { param([int]$ProcessId) return $false } `
    -Sleep { param([int]$Seconds) }
Assert-equal 1 $killLog.Count '3. Immediate force-kills exactly once'
Assert-equal 7777 $killLog[0] '3. Immediate force-kills re-verified PID 7777'
Assert-True $resultImmediate.Forced '3. Immediate Forced=true'

# 4. PID change between checks aborts
$aborted = $false
try {
    $flip = @{ N = 0 }
    Invoke-BackupServiceStop -ServiceName 'SearchEngineBackupService' `
        -InstanceId 'default' -StopMode Immediate -ImmediateGraceSeconds 0 `
        -CurrentProcessId 111 `
        -GetRuntimeInfo {
            param([string]$ServiceName)
            $flip.N++
            if ($flip.N -eq 1) { return New-FakeRuntime -ProcessId 5001 }
            if ($flip.N -eq 2) { return New-FakeRuntime -ProcessId 5001 } # after grace
            if ($flip.N -eq 3) { return New-FakeRuntime -ProcessId 5001 } # pid check 1
            return New-FakeRuntime -ProcessId 5002 # pid check 2 changed
        } `
        -RequestStop { param($n, $m) } `
        -WaitForStopped { param($n, $t) return $false } `
        -ForceStopProcess { param([int]$ProcessId) throw "should not kill $ProcessId" } `
        -Sleep { param([int]$Seconds) } | Out-Null
} catch {
    $aborted = $_.Exception.Message -match 'changed from 5001 to 5002'
}
Assert-True $aborted '4. PID change between checks aborts force-kill'

# 5. PID 0, 4 and current process are rejected
$c0 = Test-BackupServiceStopProcessId -ProcessId 0 -CurrentProcessId 999 -ServiceName 'S' -InstanceId 'i'
$c4 = Test-BackupServiceStopProcessId -ProcessId 4 -CurrentProcessId 999 -ServiceName 'S' -InstanceId 'i'
$cSelf = Test-BackupServiceStopProcessId -ProcessId 999 -CurrentProcessId 999 -ServiceName 'S' -InstanceId 'i'
$cOk = Test-BackupServiceStopProcessId -ProcessId 1234 -CurrentProcessId 999 -ServiceName 'S' -InstanceId 'i'
Assert-True (-not $c0.Ok) '5. PID 0 rejected'
Assert-True (-not $c4.Ok) '5. PID 4 rejected'
Assert-True (-not $cSelf.Ok) '5. Current process PID rejected'
Assert-True $cOk.Ok '5. Normal PID accepted'

# 6. Already stopped service is success
$already = Invoke-BackupServiceStop -ServiceName 'SearchEngineBackupService' `
    -InstanceId 'default' -StopMode Graceful `
    -GetRuntimeInfo { param($n) New-FakeRuntime -State Stopped -ProcessId 0 } `
    -RequestStop { param($n, $m) throw 'should not stop already stopped' } `
    -ForceStopProcess { param($p) throw 'should not force already stopped' }
Assert-equal 'AlreadyStopped' $already.Status '6. Already stopped is success'

# 7. User cancel does not change state
$cancel = Resolve-BackupServiceStopMode -CanPrompt $true -PromptReader { '0' } `
    -ServiceName 'SearchEngineBackupService' -InstanceId 'default'
Assert-True $cancel.Cancelled '7. Choice 0 cancels'
Assert-True ($null -eq $cancel.Mode) '7. Cancel has no mode'

# 8. Non-interactive without StopMode selects Graceful
$ni = Resolve-BackupServiceStopMode -CanPrompt $false `
    -ServiceName 'SearchEngineBackupService' -InstanceId 'default'
Assert-equal 'Graceful' $ni.Mode '8. Non-interactive default is Graceful'
Assert-equal 'NonInteractiveDefault' $ni.Source '8. Non-interactive source'

# 9. Restart-style guard: do not start while old PID exists
$oldPidStillAlive = -not (Wait-BackupServiceProcessExit -ProcessId 4242 -TimeoutSeconds 0 `
    -TestProcessExists { param([int]$ProcessId) return $true } `
    -Sleep { param([int]$Seconds) })
Assert-True $oldPidStillAlive '9. Old PID still present blocks start'
$oldPidGone = Wait-BackupServiceProcessExit -ProcessId 4242 -TimeoutSeconds 0 `
    -TestProcessExists { param([int]$ProcessId) return $false } `
    -Sleep { param([int]$Seconds) }
Assert-True $oldPidGone '9. Old PID exit allows start'

# 10. Uninstall data-preservation contract (script text check; no real uninstall)
$uninstallPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'Uninstall-BackupService.ps1'
$uninstallText = Get-Content -LiteralPath $uninstallPath -Raw
Assert-True ($uninstallText -match 'were not deleted') `
    '10. Uninstall script states user data is not deleted'
Assert-True ($uninstallText -notmatch 'Remove-Item') `
    '10. Uninstall script does not Remove-Item user data'
Assert-True ($uninstallText -match 'StopMode') `
    '10. Uninstall script supports StopMode'

# Explicit Immediate parameter path
$explicit = Resolve-BackupServiceStopMode -StopMode Immediate -CanPrompt $true `
    -PromptReader { throw 'must not prompt when StopMode is set' }
Assert-equal 'Immediate' $explicit.Mode 'Extra: explicit StopMode skips prompt'
Assert-equal 'Parameter' $explicit.Source 'Extra: explicit StopMode source=Parameter'

# Share Process rejected for Immediate
$shareRejected = $false
try {
    Invoke-BackupServiceStop -ServiceName 'SearchEngineBackupService' `
        -InstanceId 'default' -StopMode Immediate -ImmediateGraceSeconds 0 `
        -CurrentProcessId 111 `
        -GetRuntimeInfo { param($n) New-FakeRuntime -ServiceType 'Share Process' -ProcessId 8888 } `
        -RequestStop { param($n, $m) } `
        -ForceStopProcess { param($p) throw 'should not kill share process' } `
        -Sleep { param($s) } | Out-Null
} catch {
    $shareRejected = $_.Exception.Message -match 'SERVICE_WIN32_OWN_PROCESS'
}
Assert-True $shareRejected 'Extra: Share Process rejected for Immediate'

Write-Host ""
Write-Host "Passed: $script:passed  Failed: $($script:failures.Count)"
if ($script:failures.Count -gt 0) {
    Write-Host 'Failures:'
    foreach ($f in $script:failures) {
        Write-Host " - $f"
    }
    exit 1
}

Write-Host 'All mock stop-mode tests passed.'
exit 0
