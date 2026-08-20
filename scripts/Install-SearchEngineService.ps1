[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$DataDir,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId = 'default',

    [ValidateSet('AutomaticDelayedStart', 'Automatic', 'Manual')]
    [string]$StartupType = 'AutomaticDelayedStart',

    [System.Management.Automation.PSCredential]$Credential,
    [switch]$UseLocalSystem,
    [switch]$AllowDebugBinary,
    [switch]$AddFirewallRule,
    [switch]$Start
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
$description = "ASIO search and indexing server; instance=$InstanceId"

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

function Resolve-RequiredPath([string]$Path, [string]$Kind) {
    if (-not [System.IO.Path]::IsPathRooted($Path) -or
        [System.IO.Path]::GetPathRoot($Path) -eq '\') {
        throw "Path must be absolute: $Path"
    }
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "Required $Kind does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Quote-ServiceArgument([string]$Value) {
    if ($Value.Contains('"')) {
        throw "Double quote is not supported in a service argument: $Value"
    }
    return '"' + $Value + '"'
}

function Invoke-Sc([string[]]$Arguments) {
    & sc.exe @Arguments | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
}

function Read-JsonFile([string]$Path) {
    $jsonText = [System.IO.File]::ReadAllText(
        $Path,
        [System.Text.Encoding]::UTF8
    )
    if (Get-Command ConvertFrom-Json -ErrorAction SilentlyContinue) {
        return $jsonText | ConvertFrom-Json
    }

    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    return $serializer.DeserializeObject($jsonText)
}

function Test-FirewallRule([string]$RuleName) {
    if (Get-Command Get-NetFirewallRule -ErrorAction SilentlyContinue) {
        return [bool](Get-NetFirewallRule -DisplayName $RuleName `
            -ErrorAction SilentlyContinue)
    }
    & netsh.exe advfirewall firewall show rule name="$RuleName" > $null 2>&1
    return $LASTEXITCODE -eq 0
}

function Add-FirewallRule(
    [string]$RuleName,
    [int]$Port,
    [string]$Program
) {
    if (Get-Command New-NetFirewallRule -ErrorAction SilentlyContinue) {
        New-NetFirewallRule -DisplayName $RuleName -Direction Inbound `
            -Action Allow -Protocol TCP -LocalPort $Port `
            -Program $Program | Out-Null
        return
    }
    & netsh.exe advfirewall firewall add rule name="$RuleName" `
        dir=in action=allow protocol=TCP localport=$Port `
        program="$Program" enable=yes | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot add Windows Firewall rule: $RuleName"
    }
}

function Test-MappedDrivePath([string]$ConfiguredPath) {
    if ($ConfiguredPath -notmatch '^[A-Za-z]:[\/]') {
        return $false
    }
    $drive = Get-PSDrive -Name $ConfiguredPath.Substring(0, 1) `
        -ErrorAction SilentlyContinue
    return [bool]($drive -and $drive.DisplayRoot)
}

function Test-TcpPortAvailable([int]$Port) {
    $listener = New-Object System.Net.Sockets.TcpListener `
        -ArgumentList ([System.Net.IPAddress]::Any, $Port)
    try {
        $listener.Server.ExclusiveAddressUse = $true
        $listener.Start()
        return $true
    } catch {
        return $false
    } finally {
        $listener.Stop()
    }
}

Assert-Administrator

if ([bool]$Credential -eq [bool]$UseLocalSystem) {
    throw 'Choose exactly one service account option: -Credential or -UseLocalSystem. LocalSystem is never selected implicitly.'
}

$binary = Resolve-RequiredPath $BinaryPath Leaf
$data = Resolve-RequiredPath $DataDir Container

if (-not $AllowDebugBinary -and
    ($binary -match '[\/]Debug[\/]' -or
     [System.IO.Path]::GetFileNameWithoutExtension($binary) -match 'debug')) {
    throw 'A Debug binary is not registered by default. Select a Release build.'
}
if ([System.IO.Path]::GetFileName($binary) -ne 'SearchEngine.exe') {
    throw "The selected executable is not SearchEngine.exe: $binary"
}
if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
    throw "Service '$serviceName' already exists; it was not overwritten."
}

$settingsPath = Join-Path $data 'Settings.json'
$oemPath = Join-Path $data 'OEM866.INI'
$settingsPath = Resolve-RequiredPath $settingsPath Leaf
$oemPath = Resolve-RequiredPath $oemPath Leaf

$settings = Read-JsonFile $settingsPath
$config = $settings.config
$indexRootsProperty = if ($null -ne $config) {
    $config.PSObject.Properties['index_roots']
} else {
    $null
}
$dirsProperty = if ($null -ne $config) {
    $config.PSObject.Properties['dirs']
} else {
    $null
}
$indexRoots = if ($null -ne $indexRootsProperty) {
    @($indexRootsProperty.Value)
} elseif ($null -ne $dirsProperty) {
    @($dirsProperty.Value)
} else {
    @()
}
if (-not $config -or -not $config.year -or
    $indexRoots.Count -eq 0 -or
    -not $config.extensions -or $config.extensions.Count -eq 0) {
    throw "Settings.json is missing required config fields: $settingsPath"
}
$port = if ($config.port) { [int]$config.port } else { [int]$config.asio_port }
if ($port -lt 1 -or $port -gt 65535) {
    throw "Settings.json contains an invalid ASIO port: $port"
}
$documentCatalogStorage = if ($config.document_catalog_storage) {
    [string]$config.document_catalog_storage
} else {
    'memory'
}
if ($documentCatalogStorage -notin @('memory', 'sqlite')) {
    throw "Settings.json contains an invalid document_catalog_storage: $documentCatalogStorage"
}
if (-not (Test-TcpPortAvailable $port)) {
    throw "ASIO port $port is already in use. Every service instance needs a unique port."
}
$firewallRuleName = "$displayName ($port/TCP)"
if ($AddFirewallRule -and
    (Test-FirewallRule $firewallRuleName)) {
    throw "Firewall rule already exists: $firewallRuleName"
}

foreach ($configuredPath in @($indexRoots)) {
    if (Test-MappedDrivePath ([string]$configuredPath)) {
        throw "Mapped drives are not visible to services: $configuredPath. Use a local path or UNC path."
    }
    if ([string]$configuredPath -like '\\*' -and $UseLocalSystem) {
        Write-Warning "LocalSystem may not have access to UNC path: $configuredPath"
    }
}

foreach ($directory in @('logs')) {
    $path = Join-Path $data $directory
    if (-not (Test-Path -LiteralPath $path) -and
        $PSCmdlet.ShouldProcess($path, 'Create runtime directory')) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

$binaryCommand = @(
    (Quote-ServiceArgument $binary),
    '--service',
    '--service-name',
    (Quote-ServiceArgument $serviceName),
    '--data-dir',
    (Quote-ServiceArgument $data)
) -join ' '

if ($PSCmdlet.ShouldProcess($serviceName, 'Create Windows service')) {
    $newService = @{
        Name = $serviceName
        BinaryPathName = $binaryCommand
        DisplayName = $displayName
        StartupType = if ($StartupType -eq 'Manual') { 'Manual' } else { 'Automatic' }
        Description = $description
    }
    if ($Credential) {
        $newService.Credential = $Credential
    }
    New-Service @newService | Out-Null

    if ($StartupType -eq 'AutomaticDelayedStart') {
        Invoke-Sc @('config', $serviceName, 'start=', 'delayed-auto')
    }
    Invoke-Sc @('description', $serviceName, $description)
    Invoke-Sc @(
        'failure', $serviceName,
        'reset=', '86400',
        'actions=', 'restart/60000/restart/60000/restart/300000'
    )
    Invoke-Sc @('failureflag', $serviceName, '1')

    $serviceRegistry = "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName"
    New-ItemProperty -Path $serviceRegistry -Name PreshutdownTimeout `
        -PropertyType DWord -Value 1800000 -Force | Out-Null

    if ($AddFirewallRule) {
        Add-FirewallRule $firewallRuleName $port $binary
    }

    $endpointPath = Join-Path $data 'client-endpoint.txt'
    $endpointLines = @(
        "server_id=$InstanceId"
        "display_name=$displayName"
        "host=$env:COMPUTERNAME"
        "god=$($config.year)"
        "port=$port"
        "service_name=$serviceName"
    )
    [IO.File]::WriteAllLines(
        $endpointPath,
        $endpointLines,
        (New-Object Text.UTF8Encoding($false))
    )

    if ($Start) {
        Start-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Running,
            [System.TimeSpan]::FromSeconds(120)
        )
    }
}

Write-Host "Installed service '$serviceName'."
Write-Host "Instance id: $InstanceId"
Write-Host "Binary command: $binaryCommand"
Write-Host "Runtime data: $data"
Write-Host "Logs: $(Join-Path $data 'logs')"
Write-Host "Client endpoint hint: $(Join-Path $data 'client-endpoint.txt')"
Write-Host 'Verify that the selected account can read indexed paths and write the data directory.'
