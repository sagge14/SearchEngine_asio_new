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
$service = Get-Service -Name $serviceName -ErrorAction Stop

if ($PSCmdlet.ShouldProcess($serviceName, 'Restart Windows service')) {
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $serviceName
        $service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [System.TimeSpan]::FromSeconds($TimeoutSeconds)
        )
    }
    Start-Service -Name $serviceName
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Running,
        [System.TimeSpan]::FromSeconds(120)
    )
}

Get-Service -Name $serviceName
