[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [ValidateSet('Graceful', 'Immediate')]
    [string]$StopMode,

    [int]$TimeoutSeconds = 1800,

    [ValidateRange(0, 120)]
    [int]$ImmediateGraceSeconds = 2
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BackupServiceStop.Common.ps1')

$serviceName = Get-BackupServiceNameFromInstanceId -InstanceId $InstanceId
$dataDir = Join-Path $env:ProgramData $serviceName
$logsDir = Join-Path $dataDir 'logs'

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object `
        -TypeName System.Security.Principal.WindowsPrincipal `
        -ArgumentList $identity
    if (-not $principal.IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator
    )) {
        throw 'Run this script from an elevated PowerShell session.'
    }
}

function Get-ServiceDataDir([string]$Name) {
    $cim = Get-CimInstance -ClassName Win32_Service -Filter "Name='$Name'" `
        -ErrorAction SilentlyContinue
    if (-not $cim -or [string]::IsNullOrWhiteSpace($cim.PathName)) {
        return $null
    }
    if ($cim.PathName -match '--data-dir\s+"([^"]+)"') {
        return $Matches[1]
    }
    if ($cim.PathName -match '--data-dir\s+(\S+)') {
        return $Matches[1].Trim('"')
    }
    return $null
}

Assert-Administrator

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    throw "Service '$serviceName' (InstanceId=$InstanceId) is not installed."
}

$resolvedDataDir = Get-ServiceDataDir $serviceName
if ($resolvedDataDir) {
    $dataDir = $resolvedDataDir
    $logsDir = Join-Path $dataDir 'logs'
}

Write-Host "Instance: $InstanceId ($serviceName)"
Write-Host "Installed config: $(Join-Path $dataDir 'Backup.json')"

$resolved = Resolve-BackupServiceStopMode -StopMode $StopMode `
    -ServiceName $serviceName -InstanceId $InstanceId
if ($resolved.Cancelled) {
    Write-Host (
        "Stop cancelled by user for '$serviceName' (InstanceId=$InstanceId). " +
        'Service state was not changed.'
    )
    Get-Service -Name $serviceName
    return
}

$StopMode = $resolved.Mode
Write-Host "Selected StopMode=$StopMode (source=$($resolved.Source))"

try {
    $null = Invoke-BackupServiceStop -ServiceName $serviceName -InstanceId $InstanceId `
        -StopMode $StopMode -TimeoutSeconds $TimeoutSeconds `
        -ImmediateGraceSeconds $ImmediateGraceSeconds
} catch {
    Write-Host (
        "ERROR: Stop failed for '$serviceName' (InstanceId=$InstanceId, " +
        "StopMode=$StopMode)."
    )
    Write-Host $_.Exception.Message
    & sc.exe queryex $serviceName
    Write-Host "Logs: $logsDir"
    throw
}

Write-Host "Logs: $logsDir"
Write-Host 'Note: Automatic / Delayed Start services will start again after reboot.'
Write-Host 'For lasting disable, set Startup Type to Manual or Disabled separately.'
Write-Host 'Stop -> Start creates a new process and re-reads the installed Backup.json.'
Get-Service -Name $serviceName
