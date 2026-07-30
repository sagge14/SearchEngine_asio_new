[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$BinaryPath,

    [Parameter(Mandatory)]
    [string]$ConfigPath,

    [Parameter(Mandatory)]
    [string]$DataDir,

    [string]$ServiceName = 'SearchEngineBackupService',
    [string]$DisplayName = 'SearchEngine Backup Service',

    [ValidateSet('AutomaticDelayedStart', 'Automatic', 'Manual')]
    [string]$StartupType = 'AutomaticDelayedStart',

    [PSCredential]$Credential,
    [switch]$AllowDebugBinary,
    [switch]$Start
)

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )) {
        throw 'Run this script from an elevated PowerShell session.'
    }
}

function Resolve-RequiredPath([string]$Path, [string]$Kind) {
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "Required $Kind does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Quote-ServiceArgument([string]$Value) {
    if ($Value.Contains('"')) {
        throw "Double quote is not supported in service argument: $Value"
    }
    return '"' + $Value + '"'
}

function Invoke-Sc([string[]]$Arguments) {
    & sc.exe @Arguments | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
}

function Test-MappedDrivePath([string]$ConfiguredPath) {
    if ($ConfiguredPath -match '^[A-Za-z]:[\\/]') {
        $drive = Get-PSDrive -Name $ConfiguredPath.Substring(0, 1) `
            -ErrorAction SilentlyContinue
        if ($drive -and $drive.DisplayRoot) {
            return $true
        }
        try {
            return (
                [IO.DriveInfo]::new($ConfiguredPath.Substring(0, 3)).DriveType `
                    -eq [IO.DriveType]::Network
            )
        } catch {
            return $false
        }
    }
    return $false
}

Assert-Administrator
$binary = Resolve-RequiredPath $BinaryPath Leaf
$config = Resolve-RequiredPath $ConfigPath Leaf

if (-not $AllowDebugBinary -and
    ($binary -match '[\\/]Debug[\\/]' -or
     [IO.Path]::GetFileNameWithoutExtension($binary) -match 'debug')) {
    throw 'A Debug binary is not registered by default. Use a Release build.'
}
if ([IO.Path]::GetFileName($binary) -ne 'BackupService.exe') {
    Write-Warning "The selected executable is not named BackupService.exe: $binary"
}

$data = [IO.Path]::GetFullPath($DataDir)
if (-not (Test-Path -LiteralPath $data)) {
    if ($PSCmdlet.ShouldProcess($data, 'Create data directory')) {
        New-Item -ItemType Directory -Path $data | Out-Null
    }
}

if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    throw "Service '$ServiceName' already exists; it was not overwritten."
}

$json = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
if (-not $json.BackupJobs -or $json.BackupJobs.Count -eq 0) {
    throw "Configuration has no BackupJobs: $config"
}

$configuredPaths = @()
foreach ($job in $json.BackupJobs) {
    $configuredPaths += [string]$job.backup_dir
    foreach ($target in $job.targets) {
        $configuredPaths += [string]$target.src
    }
}
foreach ($configuredPath in $configuredPaths) {
    if (Test-MappedDrivePath $configuredPath) {
        throw "Mapped network drive paths are not supported for services: $configuredPath. Use UNC and an account with network access."
    }
    if ($configuredPath -like '\\*' -and -not $Credential) {
        Write-Warning "UNC path with LocalSystem may be inaccessible: $configuredPath. Consider -Credential."
    }
}

$otherInstances = Get-CimInstance Win32_Service |
    Where-Object {
        $_.Name -ne $ServiceName -and
        $_.PathName -like '*BackupService.exe*'
    }
if ($otherInstances) {
    Write-Warning (
        'Other BackupService instances exist. Do not run Backup.all.json ' +
        'together with overlapping Backup.databases.json/Backup.programs.json profiles: ' +
        (($otherInstances.Name | Sort-Object) -join ', ')
    )
}

$binaryCommand = @(
    (Quote-ServiceArgument $binary),
    '--service',
    '--service-name',
    (Quote-ServiceArgument $ServiceName),
    '--config',
    (Quote-ServiceArgument $config),
    '--data-dir',
    (Quote-ServiceArgument $data)
) -join ' '

if ($PSCmdlet.ShouldProcess($ServiceName, 'Create Windows service')) {
    $newService = @{
        Name = $ServiceName
        BinaryPathName = $binaryCommand
        DisplayName = $DisplayName
        StartupType = if ($StartupType -eq 'Manual') { 'Manual' } else { 'Automatic' }
        Description = 'Scheduled snapshot and SQLite backup service'
    }
    if ($Credential) {
        $newService.Credential = $Credential
    }
    New-Service @newService | Out-Null

    if ($StartupType -eq 'AutomaticDelayedStart') {
        Invoke-Sc @('config', $ServiceName, 'start=', 'delayed-auto')
    }
    Invoke-Sc @(
        'description',
        $ServiceName,
        'Scheduled snapshot and SQLite backup service'
    )
    Invoke-Sc @(
        'failure',
        $ServiceName,
        'reset=',
        '86400',
        'actions=',
        'restart/60000/restart/60000/restart/300000'
    )
    Invoke-Sc @('failureflag', $ServiceName, '1')

    $serviceRegistry =
        "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    New-ItemProperty `
        -Path $serviceRegistry `
        -Name PreshutdownTimeout `
        -PropertyType DWord `
        -Value 1800000 `
        -Force | Out-Null

    if ($Start) {
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus(
            [ServiceProcess.ServiceControllerStatus]::Running,
            [TimeSpan]::FromSeconds(60)
        )
    }
}

Write-Host "Installed service '$ServiceName'."
Write-Host "Binary command: $binaryCommand"
Write-Host "Logs: $(Join-Path $data 'logs')"
