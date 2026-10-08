#Requires -Version 5.1
<#
.SYNOPSIS
  Bump SearchEngine CMake product app-version once before Release compile.

.DESCRIPTION
  Invoked from a per-product CMake custom target when
  SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD is ON. Bumps patch and regenerates
  cmake/generated VERSIONINFO resources so the subsequent compile embeds the
  new PE version.

  Guards:
  - Non-Release configurations: no-op.
  - SEARCHENGINE_VERSION_BUMP_MODE=skip: no-op (Build-*Package already bumped,
    or caller used -SkipVersionBump while CMake packaging targets remain ON).
  - SEARCHENGINE_VERSION_BUMPED_<Product> process env: no-op if this process
    already bumped the product (defense in depth).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ProjectRoot,

    [Parameter(Mandatory)]
    [ValidateSet('SearchEngineService', 'BackupService', 'ZagEditor', 'BackupRestore')]
    [string]$ProductName,

    [Parameter(Mandatory)]
    [string]$Configuration
)

$ErrorActionPreference = 'Stop'

if ($Configuration -ne 'Release') {
    Write-Host (
        "Release version bump skipped for $ProductName " +
        "(Configuration=$Configuration)."
    )
    exit 0
}

$bumpMode = [string]$env:SEARCHENGINE_VERSION_BUMP_MODE
if ($bumpMode -eq 'skip') {
    Write-Host (
        "Release version bump skipped for $ProductName " +
        "(SEARCHENGINE_VERSION_BUMP_MODE=skip)."
    )
    exit 0
}

$processGuardName = "SEARCHENGINE_VERSION_BUMPED_$ProductName"
$already = [Environment]::GetEnvironmentVariable($processGuardName)
if (-not [string]::IsNullOrWhiteSpace($already)) {
    Write-Host (
        "Release version bump already applied for $ProductName in this process " +
        "($already)."
    )
    exit 0
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')

$versionInfo = Sync-SearchEngineAppVersion `
    -ProjectRoot $ProjectRoot `
    -ProductName $ProductName `
    -BumpPatch
$versionText = [string]$versionInfo.Version
[Environment]::SetEnvironmentVariable($processGuardName, $versionText)
Write-Host (
    "Release version bump applied for $ProductName -> $versionText " +
    "(resources regenerated before compile)."
)
