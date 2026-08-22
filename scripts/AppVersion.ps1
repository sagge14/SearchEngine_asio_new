#Requires -Version 5.1
<#
.SYNOPSIS
  Shared helpers for CMake app-version sync / PE checks / package naming.
#>

# Do not enable Set-StrictMode here: this file is dot-sourced into legacy
# packagers that read optional JSON properties.
$ErrorActionPreference = 'Stop'

function Resolve-SearchEngineReleaseScript {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$ScriptName
    )

    $resolveScript = & (Join-Path $PSScriptRoot 'Find-WorkspaceReleaseRoot.ps1') `
        -Name 'Resolve-WorkspaceReleaseScript.ps1' -StartPath $ProjectRoot

    $resolved = & $resolveScript -ScriptName $ScriptName -StartPath $ProjectRoot
    if ([string]::IsNullOrWhiteSpace($resolved)) {
        throw "$ScriptName not found under TOOLS\scripts\release."
    }
    return $resolved
}

function Get-SearchEngineProductBinaries {
    param([Parameter(Mandatory)][string]$ProductName)

    switch ($ProductName) {
        'SearchEngineService' {
            return @(
                @{
                    OriginalFilename = 'SearchEngine.exe'
                    FileDescription  = 'SearchEngine Windows service'
                },
                @{
                    OriginalFilename = 'SearchEngineConfig.exe'
                    FileDescription  = 'SearchEngine portable configuration helper'
                },
                @{
                    OriginalFilename = 'SearchEngineArchive.exe'
                    FileDescription  = 'SearchEngine annual archive and migration helper'
                }
            )
        }
        'BackupService' {
            return @(
                @{
                    OriginalFilename = 'BackupService.exe'
                    FileDescription  = 'BackupService Windows service'
                }
            )
        }
        'ZagEditor' {
            return @(
                @{
                    OriginalFilename = 'ZagEditor.exe'
                    FileDescription  = 'ZagEditor utility'
                }
            )
        }
        'BackupRestore' {
            return @(
                @{
                    OriginalFilename = 'BackupRestore.exe'
                    FileDescription  = 'BackupRestore CLI'
                }
            )
        }
        default {
            throw "Unknown product for version binaries: $ProductName"
        }
    }
}

function Sync-SearchEngineAppVersion {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$ProductName,
        [switch]$BumpPatch,
        [string]$Version
    )

    $syncScript = Resolve-SearchEngineReleaseScript `
        -ProjectRoot $ProjectRoot `
        -ScriptName 'Sync-CmakeProjectVersion.ps1'
    $syncArgs = @{
        ProjectRoot = $ProjectRoot
        ProductName = $ProductName
        Binaries    = @(Get-SearchEngineProductBinaries -ProductName $ProductName)
    }
    if (-not [string]::IsNullOrWhiteSpace($Version)) {
        $syncArgs.Version = $Version
    }
    elseif ($BumpPatch) {
        $syncArgs.BumpPatch = $true
    }
    $versionInfo = & $syncScript @syncArgs
    if ($null -eq $versionInfo -or [string]::IsNullOrWhiteSpace($versionInfo.Version)) {
        throw "Version sync returned no Version for $ProductName."
    }
    return $versionInfo
}

function Get-SearchEngineAppVersionNames {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$ProductName
    )

    $getScript = Resolve-SearchEngineReleaseScript `
        -ProjectRoot $ProjectRoot `
        -ScriptName 'Get-CmakeAppVersionNames.ps1'
    $versionInfo = & $getScript -ProjectRoot $ProjectRoot -ProductName $ProductName
    if ($null -eq $versionInfo -or [string]::IsNullOrWhiteSpace($versionInfo.Version)) {
        throw "Unable to read app version for $ProductName."
    }
    return $versionInfo
}

function Assert-PeMatchesAppVersion {
    param(
        [Parameter(Mandatory)][string]$BinaryPath,
        [Parameter(Mandatory)][string]$ExpectedProductVersion,
        [Parameter(Mandatory)][string]$ExpectedFileVersion,
        [string]$ExpectedProductName,
        [string]$ExpectedOriginalFilename
    )

    if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
        throw "Binary not found for PE version check: $BinaryPath"
    }

    $vi = (Get-Item -LiteralPath $BinaryPath).VersionInfo
    $productVersion = [string]$vi.ProductVersion
    $fileVersion = [string]$vi.FileVersion
    $productName = [string]$vi.ProductName
    $originalFilename = [string]$vi.OriginalFilename

    if ([string]::IsNullOrWhiteSpace($productVersion)) {
        throw "Executable has empty ProductVersion: $BinaryPath"
    }
    if ([string]::IsNullOrWhiteSpace($fileVersion)) {
        throw "Executable has empty FileVersion: $BinaryPath"
    }

    # Some tools append a trailing revision; accept exact match only for our contract.
    if ($productVersion -ne $ExpectedProductVersion) {
        throw (
            "PE ProductVersion mismatch for $BinaryPath`: " +
            "expected '$ExpectedProductVersion', got '$productVersion'."
        )
    }
    if ($fileVersion -ne $ExpectedFileVersion) {
        throw (
            "PE FileVersion mismatch for $BinaryPath`: " +
            "expected '$ExpectedFileVersion', got '$fileVersion'."
        )
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedProductName) -and
        $productName -ne $ExpectedProductName) {
        throw (
            "PE ProductName mismatch for $BinaryPath`: " +
            "expected '$ExpectedProductName', got '$productName'."
        )
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedOriginalFilename) -and
        $originalFilename -ne $ExpectedOriginalFilename) {
        throw (
            "PE OriginalFilename mismatch for $BinaryPath`: " +
            "expected '$ExpectedOriginalFilename', got '$originalFilename'."
        )
    }
}

function Get-SearchEnginePackageOutputDirectory {
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$ReleaseId,
        [Parameter(Mandatory)][string]$PackageLeaf
    )

    return (Join-Path $ProjectRoot "out\package\$ReleaseId\$PackageLeaf")
}

function Get-SearchEngineCloudZipName {
    param(
        [Parameter(Mandatory)][string]$PackageLeaf,
        [Parameter(Mandatory)][string]$ReleaseId
    )

    return "$PackageLeaf-$ReleaseId.zip"
}

function Invoke-SearchEngineCmakeBuildWithoutVersionBump {
    <#
    .SYNOPSIS
      Run cmake --build with SEARCHENGINE_VERSION_BUMP_MODE=skip so CMake
      Ensure-ReleaseVersionBump.ps1 does not bump again after Build-*Package.
    #>
    param(
        [Parameter(Mandatory)]
        [scriptblock]$BuildAction
    )

    $previousMode = [Environment]::GetEnvironmentVariable(
        'SEARCHENGINE_VERSION_BUMP_MODE'
    )
    [Environment]::SetEnvironmentVariable('SEARCHENGINE_VERSION_BUMP_MODE', 'skip')
    try {
        & $BuildAction
    }
    finally {
        if ($null -eq $previousMode -or $previousMode -eq '') {
            [Environment]::SetEnvironmentVariable(
                'SEARCHENGINE_VERSION_BUMP_MODE',
                $null
            )
        }
        else {
            [Environment]::SetEnvironmentVariable(
                'SEARCHENGINE_VERSION_BUMP_MODE',
                $previousMode
            )
        }
    }
}
