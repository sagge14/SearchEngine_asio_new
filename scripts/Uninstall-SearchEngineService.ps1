[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [int]$StopTimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
$serviceName = if ($InstanceId -eq 'default') {
    'SearchEngineService'
} else {
    "SearchEngineService-$InstanceId"
}

$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object `
    -TypeName System.Security.Principal.WindowsPrincipal `
    -ArgumentList $identity
if (-not $principal.IsInRole(
    [System.Security.Principal.WindowsBuiltInRole]::Administrator
)) {
    throw 'Run this script from an elevated PowerShell session.'
}

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host "Service '$serviceName' is not installed."
    return
}

if ($service.Status -ne 'Stopped' -and
    $PSCmdlet.ShouldProcess($serviceName, 'Stop Windows service')) {
    Stop-Service -Name $serviceName
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [System.TimeSpan]::FromSeconds($StopTimeoutSeconds)
    )
}

if ($PSCmdlet.ShouldProcess($serviceName, 'Delete Windows service registration')) {
    & sc.exe delete $serviceName | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Removed service registration '$serviceName'."
Write-Host 'Settings, index, WAL/SHM files, databases, messages and logs were not deleted.'
