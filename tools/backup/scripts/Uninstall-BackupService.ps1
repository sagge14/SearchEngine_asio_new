[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [string]$ServiceName,

    [ValidateSet('Graceful', 'Immediate')]
    [string]$StopMode,

    [int]$StopTimeoutSeconds = 1800,

    [ValidateRange(0, 120)]
    [int]$ImmediateGraceSeconds = 2
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BackupServiceStop.Common.ps1')

if ([string]::IsNullOrWhiteSpace($ServiceName)) {
    $ServiceName = Get-BackupServiceNameFromInstanceId -InstanceId $InstanceId
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)) {
    throw 'Run this script from an elevated PowerShell session.'
}

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host "Service '$ServiceName' (InstanceId=$InstanceId) is not installed."
    return
}

Write-Host "Instance: $InstanceId ($ServiceName)"

$runtime = Get-BackupServiceRuntimeInfo -ServiceName $ServiceName
if ($runtime.State -ne 'Stopped') {
    $resolved = Resolve-BackupServiceStopMode -StopMode $StopMode `
        -ServiceName $ServiceName -InstanceId $InstanceId
    if ($resolved.Cancelled) {
        Write-Host (
            "Uninstall cancelled by user for '$ServiceName' (InstanceId=$InstanceId). " +
            'Service registration and data were not changed.'
        )
        Get-Service -Name $ServiceName
        return
    }

    $StopMode = $resolved.Mode
    Write-Host "Selected StopMode=$StopMode (source=$($resolved.Source))"

    if ($PSCmdlet.ShouldProcess(
        "$ServiceName (InstanceId=$InstanceId)",
        "Stop Windows service before uninstall ($StopMode)"
    )) {
        $null = Invoke-BackupServiceStop -ServiceName $ServiceName -InstanceId $InstanceId `
            -StopMode $StopMode -TimeoutSeconds $StopTimeoutSeconds `
            -ImmediateGraceSeconds $ImmediateGraceSeconds
    } else {
        Write-Host 'Stop skipped (WhatIf/ShouldProcess). Uninstall will not delete the service.'
        return
    }
} else {
    Write-Host (
        "Service '$ServiceName' (InstanceId=$InstanceId) is already STOPPED; " +
        'skipping stop-mode selection.'
    )
}

if ($PSCmdlet.ShouldProcess(
    "$ServiceName (InstanceId=$InstanceId)",
    'Delete Windows service registration'
)) {
    & sc.exe delete $ServiceName | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw (
            "sc.exe delete failed for '$ServiceName' (InstanceId=$InstanceId) " +
            "with exit code $LASTEXITCODE."
        )
    }
}

Write-Host (
    "Removed service registration '$ServiceName' (InstanceId=$InstanceId, " +
    "StopMode=$StopMode)."
)
Write-Host 'Configuration, logs, cache, snapshots and mirror history were not deleted.'
