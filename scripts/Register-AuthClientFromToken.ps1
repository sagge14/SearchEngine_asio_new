# Register a USB auth token into auth_clients.sqlite for an installed
# SearchEngineService instance (or an explicit -DataDir).
# Compatible with Windows PowerShell 2.0 (Windows 7 SP1).
#
# Prefer launching via Register-AuthClient-FromToken.bat so instance selection
# runs in cmd.exe (visible prompts). Calling SearchEngineConfig interactive
# commands from inside PowerShell often looks like a hang.

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$TokenPath,
    [string]$DataDir,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$')]
    [string]$InstanceId,
    [string]$AuthDbToolPath,
    [string]$SearchEngineConfigPath,
    [switch]$Disabled
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is PowerShell 3+; Win7 ships with 2.0.
if (-not $PSScriptRoot) {
    $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}

function Test-NullOrWhiteSpace([string]$Value) {
    # [string]::IsNullOrWhiteSpace is .NET 4+ / PowerShell 3+.
    if ([string]::IsNullOrEmpty($Value)) {
        return $true
    }
    return ($Value.Trim().Length -eq 0)
}

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if (Test-NullOrWhiteSpace $Path) {
        throw "$Label path is empty."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ExistingDirectory([string]$Path, [string]$Label) {
    if (Test-NullOrWhiteSpace $Path) {
        throw "$Label path is empty."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-Win32Service {
    param(
        [string]$Filter
    )

    # Get-CimInstance is PowerShell 3+; Win7 uses Get-WmiObject.
    if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
        if (-not (Test-NullOrWhiteSpace $Filter)) {
            return Get-CimInstance -ClassName Win32_Service -Filter $Filter `
                -ErrorAction SilentlyContinue
        }
        return Get-CimInstance -ClassName Win32_Service -ErrorAction SilentlyContinue
    }

    if (-not (Test-NullOrWhiteSpace $Filter)) {
        return Get-WmiObject -Class Win32_Service -Filter $Filter `
            -ErrorAction SilentlyContinue
    }
    return Get-WmiObject -Class Win32_Service -ErrorAction SilentlyContinue
}

function Get-ServiceDataDir([string]$ServiceName) {
    $svc = Get-Win32Service -Filter "Name='$ServiceName'"
    if (-not $svc -or (Test-NullOrWhiteSpace $svc.PathName)) {
        return $null
    }
    if ($svc.PathName -match '--data-dir\s+"([^"]+)"') {
        return $Matches[1]
    }
    if ($svc.PathName -match '--data-dir\s+(\S+)') {
        return $Matches[1].Trim('"')
    }
    return $null
}

function Get-ServiceNameFromInstance([string]$Id) {
    if ($Id -eq 'default') {
        return 'SearchEngineService'
    }
    return "SearchEngineService-$Id"
}

function Resolve-ToolBesideScript([string]$FileName) {
    $candidates = @(
        (Join-Path $PSScriptRoot $FileName),
        (Join-Path (Split-Path -Parent $PSScriptRoot) "tools\$FileName"),
        (Join-Path $PSScriptRoot "..\tools\$FileName")
    )
    foreach ($candidate in $candidates) {
        $full = [IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            return $full
        }
    }
    return $null
}

function Select-InstalledInstanceFallback {
    Write-Host 'Looking up installed SearchEngine services...'
    # Filtered WMI query — enumerating every Win32_Service is slow on older PCs.
    $services = @(Get-Win32Service -Filter `
        "Name='SearchEngineService' OR Name LIKE 'SearchEngineService-%'" |
        Sort-Object Name)

    if ($services.Count -eq 0) {
        throw 'No installed SearchEngine services were found.'
    }

    if ($services.Count -eq 1) {
        $name = $services[0].Name
        Write-Host "Using the only installed service: $name"
        if ($name -eq 'SearchEngineService') {
            return 'default'
        }
        return $name.Substring('SearchEngineService-'.Length)
    }

    Write-Host ''
    Write-Host 'Select the SearchEngine service to register the auth client:'
    for ($i = 0; $i -lt $services.Count; $i++) {
        $svc = $services[$i]
        Write-Host ("  {0} - {1}" -f ($i + 1), $svc.Name)
        if ($svc.DisplayName -and $svc.DisplayName -ne $svc.Name) {
            Write-Host ("      Services display name: {0}" -f $svc.DisplayName)
        }
    }
    Write-Host '  0 - Cancel'

    for (;;) {
        $answer = Read-Host 'Select'
        if ($answer -eq '0') {
            throw 'Instance selection was cancelled.'
        }
        $parsed = 0
        if ([int]::TryParse($answer, [ref]$parsed) -and
            $parsed -ge 1 -and $parsed -le $services.Count)
        {
            $name = $services[$parsed - 1].Name
            if ($name -eq 'SearchEngineService') {
                return 'default'
            }
            return $name.Substring('SearchEngineService-'.Length)
        }
        Write-Host 'Enter a number from the list.'
    }
}

function Select-TokenPathInteractive {
    $defaultToken = 'E:\searchclient-auth-token.json'
    Write-Host ''
    Write-Host 'Select the USB auth token file (searchclient-auth-token.json).'
    if (Test-Path -LiteralPath $defaultToken -PathType Leaf) {
        Write-Host "Default token found: $defaultToken"
        $useDefault = Read-Host 'Use this token? [Y/n]'
        if ((Test-NullOrWhiteSpace $useDefault) -or
            $useDefault -match '^(y|yes)$')
        {
            return (Resolve-Path -LiteralPath $defaultToken).Path
        }
    }

    Write-Host 'Enter the full path to searchclient-auth-token.json,'
    Write-Host 'or press Enter to open a file dialog.'
    $typed = Read-Host 'Token path'
    if (-not (Test-NullOrWhiteSpace $typed)) {
        return $typed.Trim('"')
    }

    Write-Host 'Opening file dialog (check behind other windows if it is not visible)...'
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select searchclient-auth-token.json'
    $dialog.Filter =
        'Auth token (searchclient-auth-token.json)|searchclient-auth-token.json|JSON (*.json)|*.json|All files (*.*)|*.*'
    $dialog.FileName = 'searchclient-auth-token.json'
    if (Test-Path -LiteralPath 'E:\' -PathType Container) {
        $dialog.InitialDirectory = 'E:\'
    }

    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    $owner.ShowInTaskbar = $false
    $owner.WindowState = 'Minimized'
    try {
        if ($dialog.ShowDialog($owner) -ne [System.Windows.Forms.DialogResult]::OK) {
            throw 'Token selection was cancelled.'
        }
        return $dialog.FileName
    } finally {
        $owner.Dispose()
    }
}

try {
    Write-Host 'Resolving AuthDbTool...'
    if (Test-NullOrWhiteSpace $AuthDbToolPath) {
        $AuthDbToolPath = Resolve-ToolBesideScript 'AuthDbTool.exe'
    }
    if (Test-NullOrWhiteSpace $AuthDbToolPath) {
        throw 'AuthDbTool.exe was not found. Pass -AuthDbToolPath or place it next to this script / in tools\.'
    }
    $AuthDbToolPath = Resolve-ExistingFile $AuthDbToolPath 'AuthDbTool.exe'

    if (Test-NullOrWhiteSpace $DataDir) {
        if (Test-NullOrWhiteSpace $InstanceId) {
            # Do not call SearchEngineConfig interactive UI from PowerShell — prompts
            # often do not show and look like a hang. Use the console/WMI path.
            $InstanceId = Select-InstalledInstanceFallback
        }

        Write-Host "Resolving data directory for instance '$InstanceId'..."
        $serviceName = Get-ServiceNameFromInstance $InstanceId
        $resolved = Get-ServiceDataDir $serviceName
        if (-not (Test-NullOrWhiteSpace $resolved)) {
            $DataDir = $resolved
        } else {
            $DataDir = Join-Path $env:ProgramData $serviceName
        }
    }

    $DataDir = Resolve-ExistingDirectory $DataDir 'Data directory'
    $dbPath = Join-Path $DataDir 'auth_clients.sqlite'

    if (Test-NullOrWhiteSpace $TokenPath) {
        $TokenPath = Select-TokenPathInteractive
    }
    $TokenPath = Resolve-ExistingFile $TokenPath 'Token file'

    $instanceLabel = '(explicit data-dir)'
    if (-not (Test-NullOrWhiteSpace $InstanceId)) {
        $instanceLabel = $InstanceId
    }
    Write-Host ''
    Write-Host "Instance: $instanceLabel"
    Write-Host "Data dir: $DataDir"
    Write-Host "Auth DB:  $dbPath"
    Write-Host "Token:    $TokenPath"
    Write-Host 'Registering client...'

    $toolArgs = @(
        '--db', $dbPath,
        'add-from-token',
        '--token', $TokenPath
    )
    if ($Disabled) {
        $toolArgs += '--disabled'
    }

    if (-not $PSCmdlet.ShouldProcess($dbPath, "Register auth client from $TokenPath")) {
        return
    }

    & $AuthDbToolPath @toolArgs
    if ($LASTEXITCODE -ne 0) {
        throw "AuthDbTool failed with exit code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host 'Current clients:'
    & $AuthDbToolPath --db $dbPath list
    if ($LASTEXITCODE -ne 0) {
        throw "AuthDbTool list failed with exit code $LASTEXITCODE."
    }
} catch {
    $message = $_.Exception.Message
    if (Test-NullOrWhiteSpace $message) {
        $message = "$_"
    }
    Write-Host "ERROR: $message"
    exit 1
}
