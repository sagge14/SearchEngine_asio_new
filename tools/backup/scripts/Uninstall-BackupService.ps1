[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$ServiceName = 'SearchEngineBackupService',
    [int]$StopTimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)) {
    throw 'Run this script from an elevated PowerShell session.'
}

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host "Service '$ServiceName' is not installed."
    return
}

if ($service.Status -ne 'Stopped' -and
    $PSCmdlet.ShouldProcess($ServiceName, 'Stop Windows service')) {
    Stop-Service -Name $ServiceName
    $service.WaitForStatus(
        [ServiceProcess.ServiceControllerStatus]::Stopped,
        [TimeSpan]::FromSeconds($StopTimeoutSeconds)
    )
}

if ($PSCmdlet.ShouldProcess($ServiceName, 'Delete Windows service registration')) {
    & sc.exe delete $ServiceName | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Removed service registration '$ServiceName'."
Write-Host 'Configuration, logs, cache, snapshots and mirror history were not deleted.'
