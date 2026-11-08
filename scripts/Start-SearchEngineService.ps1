[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [int]$StartTimeoutSeconds = 1800,

    [ValidateRange(100, 60000)]
    [int]$HealthTimeoutMs = 10000,

    [string]$ConfigToolPath
)

$ErrorActionPreference = 'Stop'
$serviceName = if ($InstanceId -eq 'default') {
    'SearchEngineService'
} else {
    "SearchEngineService-$InstanceId"
}
$displayName = if ($InstanceId -eq 'default') {
    'Search Engine ASIO Server'
} else {
    "Search Engine ASIO Server ($InstanceId)"
}
$portableFirewallRule = "$serviceName TCP"

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
        BinaryPath = $null
        DataDir = $null
    }
    if ($info.PathName -match '^"([^"]+)"') {
        $info.BinaryPath = $Matches[1]
    } elseif ($info.PathName -match '^(\S+\.exe)') {
        $info.BinaryPath = $Matches[1]
    }
    if ($info.PathName -match '--data-dir\s+"([^"]+)"') {
        $info.DataDir = $Matches[1]
    } elseif ($info.PathName -match '--data-dir\s+(\S+)') {
        $info.DataDir = $Matches[1].Trim('"')
    }
    return [pscustomobject]$info
}

function Resolve-ConfigTool(
    [string]$ExplicitPath,
    [string]$BinaryPath,
    [string]$Name
) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $candidates.Add($ExplicitPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($BinaryPath)) {
        $binDir = Split-Path -Parent $BinaryPath
        $installRoot = Split-Path -Parent $binDir
        $candidates.Add((Join-Path $installRoot 'tools\SearchEngineConfig.exe'))
        $candidates.Add((Join-Path $binDir 'SearchEngineConfig.exe'))
    }
    $candidates.Add((Join-Path ${env:ProgramFiles} "$Name\tools\SearchEngineConfig.exe"))
    if (${env:ProgramFiles(x86)}) {
        $candidates.Add((
            Join-Path ${env:ProgramFiles(x86)} "$Name\tools\SearchEngineConfig.exe"
        ))
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw (
        'SearchEngineConfig.exe was not found. Pass -ConfigToolPath or use the ' +
        'portable package Start-SearchEngineService.bat.'
    )
}

function Get-InspectedPort([string]$Helper, [string]$SettingsPath) {
    if (-not (Test-Path -LiteralPath $SettingsPath -PathType Leaf)) {
        throw "Installed Settings.json was not found: $SettingsPath"
    }
    $output = & $Helper @('inspect', '--settings', $SettingsPath) 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw (
            "SearchEngineConfig inspect failed for '$SettingsPath' " +
            "(exit $LASTEXITCODE): $output"
        )
    }
    $portLine = @($output) | Where-Object { $_ -match '^port=' } |
        Select-Object -First 1
    if (-not $portLine -or $portLine -notmatch '^port=(\d+)$') {
        throw "SearchEngineConfig inspect did not report a port: $SettingsPath"
    }
    $port = [int]$Matches[1]
    if ($port -lt 1 -or $port -gt 65535) {
        throw "Installed Settings.json contains an invalid ASIO port: $port"
    }
    return $port
}

function Get-EndpointPort([string]$EndpointPath) {
    if (-not (Test-Path -LiteralPath $EndpointPath -PathType Leaf)) {
        return $null
    }
    foreach ($line in Get-Content -LiteralPath $EndpointPath -ErrorAction Stop) {
        if ($line -match '^port=(.+)$') {
            $raw = $Matches[1].Trim()
            $value = 0
            if ([int]::TryParse($raw, [ref]$value)) {
                return $value
            }
            return $null
        }
    }
    return $null
}

function Test-FirewallRuleExists([string]$RuleName) {
    if (Get-Command Get-NetFirewallRule -ErrorAction SilentlyContinue) {
        return [bool](Get-NetFirewallRule -DisplayName $RuleName `
            -ErrorAction SilentlyContinue)
    }
    & netsh.exe advfirewall firewall show rule name="$RuleName" > $null 2>&1
    return $LASTEXITCODE -eq 0
}

function Get-FirewallRuleLocalPort([string]$RuleName) {
    $output = & netsh.exe advfirewall firewall show rule name="$RuleName" `
        verbose 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    foreach ($line in @($output)) {
        if ($line -match '^\s*LocalPort:\s+(\S+)') {
            $raw = $Matches[1].Trim()
            $value = 0
            if ([int]::TryParse($raw, [ref]$value)) {
                return $value
            }
            return $raw
        }
    }
    return $null
}

function Write-PortConsistencyWarnings(
    [int]$SettingsPort,
    [string]$DataDir,
    [string]$DisplayName,
    [string]$PortableRuleName
) {
    $endpointPath = Join-Path $DataDir 'client-endpoint.txt'
    $endpointPort = Get-EndpointPort $endpointPath
    if ($null -ne $endpointPort -and $endpointPort -ne $SettingsPort) {
        Write-Warning (
            "Settings.json port is $SettingsPort, but client-endpoint.txt " +
            "still lists port=$endpointPort. Update the client host/port " +
            "settings manually; this script does not change the client database " +
            "or rewrite client-endpoint.txt."
        )
    } elseif (-not (Test-Path -LiteralPath $endpointPath -PathType Leaf)) {
        Write-Warning (
            "client-endpoint.txt was not found under $DataDir. After a port " +
            'change, update client settings manually.'
        )
    }

    $psRuleName = "$DisplayName ($SettingsPort/TCP)"
    $portablePort = Get-FirewallRuleLocalPort $PortableRuleName
    $hasPsRule = Test-FirewallRuleExists $psRuleName
    $portableExists = Test-FirewallRuleExists $PortableRuleName

    if ($portableExists -and $null -ne $portablePort -and
        "$portablePort" -ne "$SettingsPort") {
        Write-Warning (
            "Firewall rule '$PortableRuleName' allows LocalPort=$portablePort, " +
            "but Settings.json now uses $SettingsPort. Recreate the inbound " +
            'TCP rule for the new port; this script does not modify firewall rules.'
        )
    } elseif (-not $portableExists -and -not $hasPsRule) {
        Write-Warning (
            "No matching install firewall rule was found for port $SettingsPort " +
            "('$PortableRuleName' or '$psRuleName'). If clients connect remotely, " +
            'verify or recreate the Windows Firewall allow rule manually.'
        )
    }
}

function Invoke-HealthCheck(
    [string]$Helper,
    [int]$Port,
    [int]$TimeoutMs
) {
    $output = & $Helper @(
        'health',
        '--port', "$Port",
        '--timeout-ms', "$TimeoutMs"
    ) 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw (
            "PING/PONG health check failed on port $Port " +
            "(exit $LASTEXITCODE): $output"
        )
    }
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
$dataDir = if ($invocation.DataDir) {
    $invocation.DataDir
} else {
    Join-Path $env:ProgramData $serviceName
}
$settingsPath = Join-Path $dataDir 'Settings.json'
$logsDir = Join-Path $dataDir 'logs'
$helper = Resolve-ConfigTool $ConfigToolPath $invocation.BinaryPath $serviceName

Write-Host "Instance: $InstanceId ($serviceName)"
Write-Host "Runtime Settings.json: $settingsPath"
Write-Host "Config helper: $helper"

$port = Get-InspectedPort $helper $settingsPath
Write-Host "ASIO port from installed Settings.json: $port"
Write-PortConsistencyWarnings $port $dataDir $displayName $portableFirewallRule

$service.Refresh()
switch ($service.Status) {
    'Running' {
        Write-Host "Service '$serviceName' is already RUNNING; checking PING/PONG..."
        try {
            Invoke-HealthCheck $helper $port $HealthTimeoutMs
        } catch {
            Write-Host "ERROR: $($_.Exception.Message)"
            & sc.exe queryex $serviceName
            Write-Host "Logs: $logsDir"
            throw
        }
        Write-Host "Service '$serviceName' is RUNNING and answers PING/PONG on port $port."
        Get-Service -Name $serviceName
        return
    }
    'StartPending' {
        Write-Host "Service '$serviceName' is START_PENDING; waiting for RUNNING..."
        Wait-ServiceStatus $service 'Running' $StartTimeoutSeconds 'RUNNING'
        try {
            Invoke-HealthCheck $helper $port $HealthTimeoutMs
        } catch {
            Write-Host "ERROR: $($_.Exception.Message)"
            & sc.exe queryex $serviceName
            Write-Host "Logs: $logsDir"
            throw
        }
        Write-Host "Service '$serviceName' is RUNNING and answers PING/PONG on port $port."
        Get-Service -Name $serviceName
        return
    }
    'StopPending' {
        Write-Host "Service '$serviceName' is STOP_PENDING; waiting for STOPPED..."
        Wait-ServiceStatus $service 'Stopped' $StartTimeoutSeconds 'STOPPED'
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
    Invoke-HealthCheck $helper $port $HealthTimeoutMs
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    & sc.exe queryex $serviceName
    Write-Host "Logs: $logsDir"
    Write-Host 'Stop -> Start creates a new process and re-reads Settings.json.'
    throw
}

Write-Host "Service '$serviceName' is RUNNING and answers PING/PONG on port $port."
Write-Host "Logs: $logsDir"
Get-Service -Name $serviceName
