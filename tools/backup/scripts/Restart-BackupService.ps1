[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [string]$ServiceName,

    [ValidateSet('Graceful', 'Immediate')]
    [string]$StopMode,

    [int]$TimeoutSeconds = 1800,

    [ValidateRange(0, 120)]
    [int]$ImmediateGraceSeconds = 2,

    [int]$StartTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BackupServiceStop.Common.ps1')

if ([string]::IsNullOrWhiteSpace($ServiceName)) {
    $ServiceName = Get-BackupServiceNameFromInstanceId -InstanceId $InstanceId
}

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

Assert-Administrator

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $service) {
    throw "Service '$ServiceName' (InstanceId=$InstanceId) is not installed."
}

$resolved = Resolve-BackupServiceStopMode -StopMode $StopMode `
    -ServiceName $ServiceName -InstanceId $InstanceId
if ($resolved.Cancelled) {
    Write-Host (
        "Restart cancelled by user for '$ServiceName' (InstanceId=$InstanceId). " +
        'Service state was not changed.'
    )
    Get-Service -Name $ServiceName
    return
}

$StopMode = $resolved.Mode
Write-Host "Instance: $InstanceId ($ServiceName)"
Write-Host "Selected StopMode=$StopMode (source=$($resolved.Source))"

if (-not $PSCmdlet.ShouldProcess(
    "$ServiceName (InstanceId=$InstanceId)",
    "Restart Windows service (stop=$StopMode)"
)) {
    return
}

$runtimeBefore = Get-BackupServiceRuntimeInfo -ServiceName $ServiceName
$previousPid = [int]$runtimeBefore.ProcessId

if ($runtimeBefore.State -ne 'Stopped') {
    $null = Invoke-BackupServiceStop -ServiceName $ServiceName -InstanceId $InstanceId `
        -StopMode $StopMode -TimeoutSeconds $TimeoutSeconds `
        -ImmediateGraceSeconds $ImmediateGraceSeconds
} else {
    Write-Host (
        "Service '$ServiceName' (InstanceId=$InstanceId) is already STOPPED; " +
        'starting without a stop step.'
    )
}

if ($previousPid -gt 4) {
    Write-Host "Waiting for previous PID $previousPid to exit before start..."
    $exited = Wait-BackupServiceProcessExit -ProcessId $previousPid -TimeoutSeconds 30
    if (-not $exited) {
        throw (
            "Cannot start '$ServiceName' (InstanceId=$InstanceId, StopMode=$StopMode): " +
            "previous PID $previousPid is still running."
        )
    }
}

# Final guard: service must be STOPPED and no longer own the old PID.
$runtimeAfterStop = Get-BackupServiceRuntimeInfo -ServiceName $ServiceName
if ($runtimeAfterStop.State -ne 'Stopped') {
    throw (
        "Cannot start '$ServiceName' (InstanceId=$InstanceId, StopMode=$StopMode): " +
        "service state is '$($runtimeAfterStop.State)', expected STOPPED."
    )
}
if ($previousPid -gt 4 -and [int]$runtimeAfterStop.ProcessId -eq $previousPid) {
    throw (
        "Cannot start '$ServiceName' (InstanceId=$InstanceId, StopMode=$StopMode): " +
        "service still reports previous PID $previousPid."
    )
}

Write-Host "Starting '$ServiceName' (InstanceId=$InstanceId)..."
Start-Service -Name $ServiceName -ErrorAction Stop
$service.Refresh()
try {
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Running,
        [System.TimeSpan]::FromSeconds($StartTimeoutSeconds)
    )
} catch {
    throw (
        "Service '$ServiceName' (InstanceId=$InstanceId) did not reach RUNNING " +
        "within ${StartTimeoutSeconds}s after StopMode=$StopMode."
    )
}

Write-Host "Service '$ServiceName' (InstanceId=$InstanceId) is RUNNING."
Get-Service -Name $ServiceName
