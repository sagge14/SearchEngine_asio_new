# Register a USB auth token into auth_clients.sqlite for an installed
# SearchEngineService instance (or an explicit --DataDir).

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

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label path is empty."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ExistingDirectory([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label path is empty."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-ServiceDataDir([string]$ServiceName) {
    $cim = Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'" `
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

function Select-InstalledInstanceInteractive([string]$ConfigTool) {
    $output = Join-Path $env:TEMP (
        'SearchEngine-AuthRegister-' + [Guid]::NewGuid().ToString('N') + '.txt'
    )
    try {
        & $ConfigTool choose-installed-instance --output $output
        $exitCode = $LASTEXITCODE
        if ($exitCode -eq 3) {
            throw 'No installed SearchEngine services were found.'
        }
        if ($exitCode -eq 2) {
            throw 'Instance selection was cancelled.'
        }
        if ($exitCode -ne 0) {
            throw "SearchEngineConfig choose-installed-instance failed ($exitCode)."
        }
        $selected = $null
        Get-Content -LiteralPath $output -Encoding UTF8 | ForEach-Object {
            if ($_ -match '^instance=(.+)$') {
                $selected = $Matches[1].Trim()
            }
        }
        if ([string]::IsNullOrWhiteSpace($selected)) {
            throw 'SearchEngineConfig did not return an instance id.'
        }
        return $selected
    } finally {
        if (Test-Path -LiteralPath $output) {
            Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
        }
    }
}

function Select-InstalledInstanceFallback {
    $services = @(Get-CimInstance -ClassName Win32_Service -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -eq 'SearchEngineService' -or
            $_.Name -like 'SearchEngineService-*'
        } |
        Sort-Object Name)

    if ($services.Count -eq 0) {
        throw 'No installed SearchEngine services were found.'
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
    if (Test-Path -LiteralPath $defaultToken -PathType Leaf) {
        Write-Host ''
        Write-Host "Default token found: $defaultToken"
        $useDefault = Read-Host 'Use this token? [Y/n]'
        if ([string]::IsNullOrWhiteSpace($useDefault) -or
            $useDefault -match '^(y|yes)$')
        {
            return (Resolve-Path -LiteralPath $defaultToken).Path
        }
    }

    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select searchclient-auth-token.json'
    $dialog.Filter =
        'Auth token (searchclient-auth-token.json)|searchclient-auth-token.json|JSON (*.json)|*.json|All files (*.*)|*.*'
    $dialog.FileName = 'searchclient-auth-token.json'
    if (Test-Path -LiteralPath 'E:\' -PathType Container) {
        $dialog.InitialDirectory = 'E:\'
    }
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw 'Token selection was cancelled.'
    }
    return $dialog.FileName
}

if (-not $AuthDbToolPath) {
    $AuthDbToolPath = Resolve-ToolBesideScript 'AuthDbTool.exe'
}
if (-not $AuthDbToolPath) {
    throw 'AuthDbTool.exe was not found. Pass -AuthDbToolPath or place it next to this script / in tools\.'
}
$AuthDbToolPath = Resolve-ExistingFile $AuthDbToolPath 'AuthDbTool.exe'

if (-not $DataDir) {
    if (-not $InstanceId) {
        if (-not $SearchEngineConfigPath) {
            $SearchEngineConfigPath = Resolve-ToolBesideScript 'SearchEngineConfig.exe'
        }
        if ($SearchEngineConfigPath -and
            (Test-Path -LiteralPath $SearchEngineConfigPath -PathType Leaf))
        {
            $InstanceId = Select-InstalledInstanceInteractive $SearchEngineConfigPath
        } else {
            Write-Host 'SearchEngineConfig.exe not found; using service list fallback.'
            $InstanceId = Select-InstalledInstanceFallback
        }
    }

    $serviceName = Get-ServiceNameFromInstance $InstanceId
    $resolved = Get-ServiceDataDir $serviceName
    if ($resolved) {
        $DataDir = $resolved
    } else {
        $DataDir = Join-Path $env:ProgramData $serviceName
    }
}

$DataDir = Resolve-ExistingDirectory $DataDir 'Data directory'
$dbPath = Join-Path $DataDir 'auth_clients.sqlite'

if (-not $TokenPath) {
    $TokenPath = Select-TokenPathInteractive
}
$TokenPath = Resolve-ExistingFile $TokenPath 'Token file'

Write-Host "Instance: $(if ($InstanceId) { $InstanceId } else { '(explicit data-dir)' })"
Write-Host "Data dir: $DataDir"
Write-Host "Auth DB:  $dbPath"
Write-Host "Token:    $TokenPath"

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
