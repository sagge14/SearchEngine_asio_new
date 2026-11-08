[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [int]$StartTimeoutSeconds = 120,

    [int]$StopPendingTimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
$serviceName = if ($InstanceId -eq 'default') {
    'SearchEngineBackupService'
} else {
    "SearchEngineBackupService-$InstanceId"
}
$dataDir = Join-Path $env:ProgramData $serviceName
$logsDir = Join-Path $dataDir 'logs'
$configPath = Join-Path $dataDir 'Backup.json'

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

function Get-ServiceInvocation([string]$Name) {
    $cim = Get-CimInstance -ClassName Win32_Service -Filter "Name='$Name'" `
        -ErrorAction Stop
    $info = [ordered]@{
        PathName = [string]$cim.PathName
        DataDir = $null
        ConfigPath = $null
    }
    if ($info.PathName -match '--data-dir\s+"([^"]+)"') {
        $info.DataDir = $Matches[1]
    } elseif ($info.PathName -match '--data-dir\s+(\S+)') {
        $info.DataDir = $Matches[1].Trim('"')
    }
    if ($info.PathName -match '--config\s+"([^"]+)"') {
        $info.ConfigPath = $Matches[1]
    } elseif ($info.PathName -match '--config\s+(\S+)') {
        $info.ConfigPath = $Matches[1].Trim('"')
    }
    return [pscustomobject]$info
}

function Wait-ServiceStatus(
    [System.ServiceProcess.ServiceController]$Service,
    [System.ServiceProcess.ServiceControllerStatus]$Status,
    [int]$TimeoutSeconds,
    [string]$Label
) {
    try {
        $Service.WaitForStatus(
            $Status,
            [System.TimeSpan]::FromSeconds($TimeoutSeconds)
        )
    } catch {
        throw "Service did not reach $Label within ${TimeoutSeconds}s."
    }
    $Service.Refresh()
}

Assert-Administrator

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    throw "Service '$serviceName' is not installed."
}

$invocation = Get-ServiceInvocation $serviceName
if ($invocation.DataDir) {
    $dataDir = $invocation.DataDir
    $logsDir = Join-Path $dataDir 'logs'
}
$configPath = if ($invocation.ConfigPath) {
    $invocation.ConfigPath
} else {
    Join-Path $dataDir 'Backup.json'
}

Write-Host "Instance: $InstanceId ($serviceName)"
Write-Host "Installed Backup.json: $configPath"
Write-Host 'The portable package data\Backup.json template is not used at runtime.'

$service.Refresh()
switch ($service.Status) {
    'Running' {
        Write-Host "Service '$serviceName' is already RUNNING."
        Write-Host "Logs: $logsDir"
        Get-Service -Name $serviceName
        return
    }
    'StartPending' {
        Write-Host "Service '$serviceName' is START_PENDING; waiting for RUNNING..."
        try {
            Wait-ServiceStatus $service 'Running' $StartTimeoutSeconds 'RUNNING'
        } catch {
            Write-Host "ERROR: $($_.Exception.Message)"
            & sc.exe queryex $serviceName
            Write-Host "Logs: $logsDir"
            throw
        }
        Write-Host "Service '$serviceName' is RUNNING."
        Write-Host "Logs: $logsDir"
        Write-Host 'Stop -> Start creates a new process and re-reads the installed Backup.json.'
        Get-Service -Name $serviceName
        return
    }
    'StopPending' {
        Write-Host "Service '$serviceName' is STOP_PENDING; waiting for STOPPED..."
        try {
            Wait-ServiceStatus $service 'Stopped' $StopPendingTimeoutSeconds 'STOPPED'
        } catch {
            Write-Host "ERROR: $($_.Exception.Message)"
            & sc.exe queryex $serviceName
            Write-Host "Logs: $logsDir"
            throw
        }
    }
    'Stopped' {
        # continue to start below
    }
    default {
        throw (
            "Service '$serviceName' is in unsupported transitional state " +
            "'$($service.Status)'. Pause/Continue is not supported; use Stop then Start."
        )
    }
}

if (-not $PSCmdlet.ShouldProcess($serviceName, 'Start Windows service')) {
    return
}

Write-Host "Starting '$serviceName'..."
try {
    Start-Service -Name $serviceName -ErrorAction Stop
    $service.Refresh()
    Wait-ServiceStatus $service 'Running' $StartTimeoutSeconds 'RUNNING'
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    & sc.exe queryex $serviceName
    Write-Host "Logs: $logsDir"
    Write-Host 'Stop -> Start creates a new process and re-reads the installed Backup.json.'
    throw
}

Write-Host "Service '$serviceName' is RUNNING."
Write-Host "Logs: $logsDir"
Write-Host 'The new process re-read the installed Backup.json via BackupServiceApplication::configure().'
Write-Host "Installed config: $configPath"
Get-Service -Name $serviceName
