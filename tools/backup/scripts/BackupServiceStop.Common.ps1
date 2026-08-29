# Shared stop helpers for BackupService management scripts.
# Dot-source this file; do not execute it directly.

Set-StrictMode -Version Latest

function Get-BackupServiceNameFromInstanceId {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
        [string]$InstanceId
    )

    if ($InstanceId -eq 'default') {
        return 'SearchEngineBackupService'
    }
    return "SearchEngineBackupService-$InstanceId"
}

function Test-BackupServiceStopCanPrompt {
    [CmdletBinding()]
    param()

    try {
        if (-not [Environment]::UserInteractive) {
            return $false
        }
        if ([Console]::IsInputRedirected) {
            return $false
        }
        return $true
    } catch {
        return $false
    }
}

function Resolve-BackupServiceStopMode {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [AllowEmptyString()]
        [string]$StopMode,

        [string]$ServiceName = 'SearchEngineBackupService',
        [string]$InstanceId = 'default',

        [nullable[bool]]$CanPrompt = $null,
        [scriptblock]$PromptReader
    )

    if (-not [string]::IsNullOrWhiteSpace($StopMode)) {
        if ($StopMode -ne 'Graceful' -and $StopMode -ne 'Immediate') {
            throw (
                "Invalid StopMode '$StopMode' for service '$ServiceName' " +
                "(InstanceId=$InstanceId). Use Graceful or Immediate."
            )
        }
        return [pscustomobject]@{
            Mode = $StopMode
            Cancelled = $false
            Source = 'Parameter'
        }
    }

    $allowPrompt = if ($null -ne $CanPrompt) {
        [bool]$CanPrompt
    } else {
        Test-BackupServiceStopCanPrompt
    }

    if (-not $allowPrompt) {
        return [pscustomobject]@{
            Mode = 'Graceful'
            Cancelled = $false
            Source = 'NonInteractiveDefault'
        }
    }

    Write-Host ""
    Write-Host "Stop mode for instance '$InstanceId' (service '$ServiceName'):"
    Write-Host '  [1] Безопасно остановить — дождаться завершения текущих задач (рекомендуется)'
    Write-Host '  [2] Остановить немедленно — прервать выполняющийся backup'
    Write-Host '  [0] Отмена'
    Write-Host 'WARNING: Immediate stop interrupts an in-progress backup; unpublished staging may remain until the next start cleans .partial_* directories.'

    $raw = if ($PromptReader) {
        & $PromptReader
    } else {
        Read-Host 'Select [1]'
    }

    if ($null -eq $raw) {
        $raw = ''
    }
    $choice = ([string]$raw).Trim()
    if ($choice -eq '') {
        $choice = '1'
    }

    switch ($choice) {
        '1' {
            return [pscustomobject]@{
                Mode = 'Graceful'
                Cancelled = $false
                Source = 'Prompt'
            }
        }
        '2' {
            return [pscustomobject]@{
                Mode = 'Immediate'
                Cancelled = $false
                Source = 'Prompt'
            }
        }
        '0' {
            return [pscustomobject]@{
                Mode = $null
                Cancelled = $true
                Source = 'Prompt'
            }
        }
        default {
            throw (
                "Invalid stop mode choice '$choice' for service '$ServiceName' " +
                "(InstanceId=$InstanceId). Use 1, 2, or 0."
            )
        }
    }
}

function Test-BackupServiceStopProcessId {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [int]$ProcessId,

        [int]$CurrentProcessId = $PID,

        [string]$ServiceName = 'SearchEngineBackupService',
        [string]$InstanceId = 'default'
    )

    if ($ProcessId -le 4) {
        return [pscustomobject]@{
            Ok = $false
            Reason = (
                "Refusing ProcessId=$ProcessId for service '$ServiceName' " +
                "(InstanceId=$InstanceId): PID must be greater than 4."
            )
        }
    }

    if ($ProcessId -eq $CurrentProcessId) {
        return [pscustomobject]@{
            Ok = $false
            Reason = (
                "Refusing ProcessId=$ProcessId for service '$ServiceName' " +
                "(InstanceId=$InstanceId): PID matches the current process."
            )
        }
    }

    return [pscustomobject]@{
        Ok = $true
        Reason = $null
    }
}

function Test-BackupServiceOwnProcessType {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string]$ServiceType,

        [string]$ServiceName = 'SearchEngineBackupService',
        [string]$InstanceId = 'default'
    )

    # Win32_Service.ServiceType for SERVICE_WIN32_OWN_PROCESS is typically
    # "Own Process". Accept a few equivalent spellings.
    $normalized = $ServiceType.Trim()
    $ok = $normalized -match '(?i)(^|\b)Own Process(\b|$)' -or
        $normalized -eq 'SERVICE_WIN32_OWN_PROCESS' -or
        $normalized -eq '16'

    if (-not $ok) {
        return [pscustomobject]@{
            Ok = $false
            Reason = (
                "Service '$ServiceName' (InstanceId=$InstanceId) has ServiceType " +
                "'$ServiceType'; Immediate stop requires SERVICE_WIN32_OWN_PROCESS."
            )
        }
    }

    return [pscustomobject]@{
        Ok = $true
        Reason = $null
    }
}

function Get-BackupServiceRuntimeInfo {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$ServiceName
    )

    $cim = Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'" `
        -ErrorAction SilentlyContinue
    if (-not $cim) {
        return [pscustomobject]@{
            Exists = $false
            State = $null
            ProcessId = 0
            ServiceType = $null
            PathName = $null
        }
    }

    return [pscustomobject]@{
        Exists = $true
        State = [string]$cim.State
        ProcessId = [int]$cim.ProcessId
        ServiceType = [string]$cim.ServiceType
        PathName = [string]$cim.PathName
    }
}

function Wait-BackupServiceStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$ServiceName,

        [Parameter(Mandatory)]
        [ValidateSet('Stopped', 'Running')]
        [string]$DesiredState,

        [int]$TimeoutSeconds = 1800,

        [scriptblock]$GetRuntimeInfo = ${function:Get-BackupServiceRuntimeInfo},
        [scriptblock]$Sleep = { param([int]$Seconds) Start-Sleep -Seconds $Seconds }
    )

    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([datetime]::UtcNow -le $deadline) {
        $info = & $GetRuntimeInfo -ServiceName $ServiceName
        if (-not $info.Exists) {
            throw "Service '$ServiceName' disappeared while waiting for $DesiredState."
        }
        if ($info.State -eq $DesiredState) {
            return $true
        }
        & $Sleep 1
    }

    return $false
}

function Wait-BackupServiceProcessExit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [int]$ProcessId,

        [int]$TimeoutSeconds = 30,

        [scriptblock]$TestProcessExists = {
            param([int]$ProcessId)
            return $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
        },
        [scriptblock]$Sleep = { param([int]$Seconds) Start-Sleep -Seconds $Seconds }
    )

    if ($ProcessId -le 4) {
        return $true
    }

    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([datetime]::UtcNow -le $deadline) {
        if (-not (& $TestProcessExists $ProcessId)) {
            return $true
        }
        & $Sleep 1
    }

    return -not (& $TestProcessExists $ProcessId)
}

function Invoke-BackupServiceStop {
    <#
    .SYNOPSIS
        Stop a BackupService instance using Graceful or Immediate mode.
    .NOTES
        Injectable scriptblocks support unit tests without touching a real service.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory)]
        [string]$ServiceName,

        [string]$InstanceId = 'default',

        [Parameter(Mandatory)]
        [ValidateSet('Graceful', 'Immediate')]
        [string]$StopMode,

        [int]$TimeoutSeconds = 1800,
        [int]$ImmediateGraceSeconds = 2,
        [int]$CurrentProcessId = $PID,

        [scriptblock]$GetRuntimeInfo = ${function:Get-BackupServiceRuntimeInfo},
        [scriptblock]$RequestStop,
        [scriptblock]$WaitForStopped,
        [scriptblock]$ForceStopProcess,
        [scriptblock]$TestProcessExists,
        [scriptblock]$Sleep = { param([int]$Seconds) Start-Sleep -Seconds $Seconds }
    )

    if (-not $RequestStop) {
        $RequestStop = {
            param([string]$Name, [string]$Mode)
            if ($Mode -eq 'Graceful') {
                $svc = Get-Service -Name $Name -ErrorAction Stop
                if ($svc.Status -ne 'StopPending') {
                    Stop-Service -Name $Name -ErrorAction Stop
                }
            } else {
                # Non-blocking SCM stop request.
                & sc.exe stop $Name | Out-Null
            }
        }
    }

    if (-not $WaitForStopped) {
        $WaitForStopped = {
            param([string]$Name, [int]$Timeout)
            return Wait-BackupServiceStatus -ServiceName $Name -DesiredState Stopped `
                -TimeoutSeconds $Timeout -GetRuntimeInfo $GetRuntimeInfo -Sleep $Sleep
        }
    }

    if (-not $ForceStopProcess) {
        $ForceStopProcess = {
            param([int]$ProcessId)
            Stop-Process -Id $ProcessId -Force -ErrorAction Stop
        }
    }

    if (-not $TestProcessExists) {
        $TestProcessExists = {
            param([int]$ProcessId)
            return $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
        }
    }

    $info = & $GetRuntimeInfo -ServiceName $ServiceName
    if (-not $info.Exists) {
        throw "Service '$ServiceName' (InstanceId=$InstanceId) is not installed."
    }

    if ($info.State -eq 'Stopped') {
        Write-Host (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=$StopMode) " +
            'is already STOPPED.'
        )
        return [pscustomobject]@{
            Status = 'AlreadyStopped'
            StopMode = $StopMode
            Forced = $false
            ProcessId = 0
            Cancelled = $false
        }
    }

    $action = "Stop Windows service ($StopMode)"
    if (-not $PSCmdlet.ShouldProcess("$ServiceName (InstanceId=$InstanceId)", $action)) {
        return [pscustomobject]@{
            Status = 'WhatIfOrSkipped'
            StopMode = $StopMode
            Forced = $false
            ProcessId = [int]$info.ProcessId
            Cancelled = $false
        }
    }

    if ($StopMode -eq 'Graceful') {
        Write-Host (
            "Stopping '$ServiceName' (InstanceId=$InstanceId, StopMode=Graceful, " +
            "timeout ${TimeoutSeconds}s)..."
        )
        Write-Host 'A long-running backup operation may delay STOPPED.'
        Write-Host 'No taskkill / Stop-Process -Force is performed in Graceful mode.'

        & $RequestStop $ServiceName 'Graceful'
        $stopped = & $WaitForStopped $ServiceName $TimeoutSeconds
        if (-not $stopped) {
            throw (
                "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Graceful) " +
                "did not reach STOPPED within ${TimeoutSeconds}s."
            )
        }

        Write-Host (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Graceful) is STOPPED."
        )
        return [pscustomobject]@{
            Status = 'Stopped'
            StopMode = 'Graceful'
            Forced = $false
            ProcessId = 0
            Cancelled = $false
        }
    }

    # Immediate mode
    Write-Host (
        "WARNING: Immediate stop for '$ServiceName' (InstanceId=$InstanceId) will " +
        'interrupt an in-progress backup.'
    )
    Write-Host (
        'Unpublished staging (.partial_*) may remain until the next service start; ' +
        'published snapshots, cache and configuration are not deleted.'
    )

    $typeCheck = Test-BackupServiceOwnProcessType -ServiceType ([string]$info.ServiceType) `
        -ServiceName $ServiceName -InstanceId $InstanceId
    if (-not $typeCheck.Ok) {
        throw $typeCheck.Reason
    }

    $initialPid = [int]$info.ProcessId
    $initialPidCheck = Test-BackupServiceStopProcessId -ProcessId $initialPid `
        -CurrentProcessId $CurrentProcessId -ServiceName $ServiceName -InstanceId $InstanceId
    if (-not $initialPidCheck.Ok) {
        throw $initialPidCheck.Reason
    }

    Write-Host (
        "Stopping '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate): " +
        "sending STOP, then waiting up to ${ImmediateGraceSeconds}s before a verified force-kill."
    )
    & $RequestStop $ServiceName 'Immediate'
    if ($ImmediateGraceSeconds -gt 0) {
        & $Sleep $ImmediateGraceSeconds
    }

    $afterGrace = & $GetRuntimeInfo -ServiceName $ServiceName
    if (-not $afterGrace.Exists) {
        throw "Service '$ServiceName' (InstanceId=$InstanceId) disappeared during Immediate stop."
    }
    if ($afterGrace.State -eq 'Stopped') {
        Write-Host (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate) " +
            'reached STOPPED without force-terminate.'
        )
        return [pscustomobject]@{
            Status = 'Stopped'
            StopMode = 'Immediate'
            Forced = $false
            ProcessId = 0
            Cancelled = $false
        }
    }

    $pidCheck1Info = & $GetRuntimeInfo -ServiceName $ServiceName
    $pid1 = [int]$pidCheck1Info.ProcessId
    $check1 = Test-BackupServiceStopProcessId -ProcessId $pid1 `
        -CurrentProcessId $CurrentProcessId -ServiceName $ServiceName -InstanceId $InstanceId
    if (-not $check1.Ok) {
        throw $check1.Reason
    }
    if ($pid1 -ne $initialPid) {
        throw (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate): " +
            "ProcessId changed from $initialPid to $pid1 between STOP and force-kill; aborting."
        )
    }

    # Second fresh query immediately before force-kill; never kill a stale saved PID alone.
    $pidCheck2Info = & $GetRuntimeInfo -ServiceName $ServiceName
    if (-not $pidCheck2Info.Exists) {
        throw "Service '$ServiceName' (InstanceId=$InstanceId) disappeared before force-kill."
    }
    if ($pidCheck2Info.State -eq 'Stopped') {
        Write-Host (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate) " +
            'reached STOPPED before force-terminate.'
        )
        return [pscustomobject]@{
            Status = 'Stopped'
            StopMode = 'Immediate'
            Forced = $false
            ProcessId = 0
            Cancelled = $false
        }
    }

    $pid2 = [int]$pidCheck2Info.ProcessId
    $check2 = Test-BackupServiceStopProcessId -ProcessId $pid2 `
        -CurrentProcessId $CurrentProcessId -ServiceName $ServiceName -InstanceId $InstanceId
    if (-not $check2.Ok) {
        throw $check2.Reason
    }
    if ($pid2 -ne $pid1) {
        throw (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate): " +
            "ProcessId changed from $pid1 to $pid2 between verification queries; aborting."
        )
    }

    $typeCheck2 = Test-BackupServiceOwnProcessType -ServiceType ([string]$pidCheck2Info.ServiceType) `
        -ServiceName $ServiceName -InstanceId $InstanceId
    if (-not $typeCheck2.Ok) {
        throw $typeCheck2.Reason
    }

    Write-Host (
        "Force-terminating verified PID $pid2 for '$ServiceName' " +
        "(InstanceId=$InstanceId, StopMode=Immediate)..."
    )
    & $ForceStopProcess $pid2

    $stopped = & $WaitForStopped $ServiceName $TimeoutSeconds
    if (-not $stopped) {
        throw (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate) " +
            "did not reach STOPPED after force-terminating PID $pid2."
        )
    }

    $exited = Wait-BackupServiceProcessExit -ProcessId $pid2 -TimeoutSeconds 30 `
        -TestProcessExists $TestProcessExists -Sleep $Sleep
    if (-not $exited) {
        throw (
            "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate) is STOPPED, " +
            "but verified PID $pid2 is still running."
        )
    }

    Write-Host (
        "Service '$ServiceName' (InstanceId=$InstanceId, StopMode=Immediate) is STOPPED " +
        "(forced PID $pid2)."
    )
    Write-Host (
        'WARNING: An interrupted backup may leave .partial_* staging until the next start.'
    )

    return [pscustomobject]@{
        Status = 'Stopped'
        StopMode = 'Immediate'
        Forced = $true
        ProcessId = $pid2
        Cancelled = $false
    }
}
