[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
$serviceName = if ($InstanceId -eq 'default') {
    'SearchEngineService'
} else {
    "SearchEngineService-$InstanceId"
}
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
    throw "Service '$serviceName' is not installed."
}

$resolvedDataDir = Get-ServiceDataDir $serviceName
if ($resolvedDataDir) {
    $dataDir = $resolvedDataDir
    $logsDir = Join-Path $dataDir 'logs'
}

if ($service.Status -eq 'Stopped') {
    Write-Host "Service '$serviceName' is already STOPPED."
    Write-Host "Logs: $logsDir"
    Get-Service -Name $serviceName
    return
}

if (-not $PSCmdlet.ShouldProcess($serviceName, 'Stop Windows service')) {
    return
}

Write-Host "Stopping '$serviceName' (graceful stop, timeout ${TimeoutSeconds}s)..."
Write-Host 'No forced process termination is performed by this script.'
try {
    Stop-Service -Name $serviceName -ErrorAction Stop
    $service.Refresh()
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [System.TimeSpan]::FromSeconds($TimeoutSeconds)
    )
} catch {
    Write-Host "ERROR: Service '$serviceName' did not reach STOPPED."
    Write-Host $_.Exception.Message
    & sc.exe queryex $serviceName
    Write-Host "Logs: $logsDir"
    Write-Host 'If a large index update is still closing, wait and retry Stop.'
    Write-Host 'Forced process termination is not offered here; request it separately if needed.'
    throw
}

Write-Host "Service '$serviceName' is STOPPED."
Write-Host "Logs: $logsDir"
Write-Host 'Note: Automatic / Delayed Start services will start again after reboot.'
Write-Host 'For lasting disable, set Startup Type to Manual or Disabled separately.'
Get-Service -Name $serviceName
