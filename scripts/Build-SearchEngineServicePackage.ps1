[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'x86-modern', 'All')]
    [string]$Architecture = 'All',
    [string]$SettingsPath,
    [string]$OutputDirectory,
    [string]$VCRedistPath,
    [switch]$SkipConfigure,
    [string]$CloudRoot,
    [string]$CloudReleaseId,
    [switch]$SkipCloudPublish,
    [switch]$SkipVersionBump
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')

$productName = 'SearchEngineService'
$versionInfo = Sync-SearchEngineAppVersion `
    -ProjectRoot $projectRoot `
    -ProductName $productName `
    -BumpPatch:(-not $SkipVersionBump)
if ([string]::IsNullOrWhiteSpace($CloudReleaseId)) {
    $CloudReleaseId = $versionInfo.ReleaseId
}

if ($Architecture -eq 'All' -and
    (-not [string]::IsNullOrWhiteSpace($OutputDirectory) -or
     -not [string]::IsNullOrWhiteSpace($VCRedistPath))) {
    throw '-OutputDirectory and -VCRedistPath require a single -Architecture.'
}

$builds = if ($Architecture -eq 'All') {
    @(
        @{ Architecture = 'x64'; Configure = 'windows-x64'; Build = 'windows-x64-release' },
        @{ Architecture = 'x86-modern'; Configure = 'windows-x86'; Build = 'windows-x86-release' },
        @{ Architecture = 'x86'; Configure = 'windows7-x86'; Build = 'windows7-x86-release' }
    )
} elseif ($Architecture -eq 'x86') {
    @(@{ Architecture = 'x86'; Configure = 'windows7-x86'; Build = 'windows7-x86-release' })
} elseif ($Architecture -eq 'x86-modern') {
    @(@{ Architecture = 'x86-modern'; Configure = 'windows-x86'; Build = 'windows-x86-release' })
} else {
    @(@{ Architecture = 'x64'; Configure = 'windows-x64'; Build = 'windows-x64-release' })
}

foreach ($build in $builds) {
    Push-Location $projectRoot
    try {
        if (-not $SkipConfigure) {
            # Build-*Package packs explicitly; disable IDE PostBuild packaging for this tree.
            & cmake --preset $build.Configure -DSEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=OFF
            if ($LASTEXITCODE -ne 0) {
                throw "CMake configure '$($build.Configure)' failed with exit code $LASTEXITCODE."
            }
        }

        & cmake --build --preset $build.Build `
            --target SearchEngine SearchEngineConfig -- /m
        if ($LASTEXITCODE -ne 0) {
            throw (
                "SearchEngine and SearchEngineConfig " +
                "$($build.Architecture) Release build failed with exit code " +
                "$LASTEXITCODE."
            )
        }
    } finally {
        Pop-Location
    }

    $packageArguments = @{
        Architecture = $build.Architecture
        SkipCloudPublish = $SkipCloudPublish
        CloudReleaseId = $CloudReleaseId
    }
    if (-not [string]::IsNullOrWhiteSpace($SettingsPath)) {
        $packageArguments.SettingsPath = $SettingsPath
    }
    if (-not [string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $packageArguments.OutputDirectory = $OutputDirectory
    }
    if (-not [string]::IsNullOrWhiteSpace($VCRedistPath)) {
        $packageArguments.VCRedistPath = $VCRedistPath
    }
    if (-not [string]::IsNullOrWhiteSpace($CloudRoot)) {
        $packageArguments.CloudRoot = $CloudRoot
    }

    & (Join-Path $PSScriptRoot 'New-SearchEngineServicePackage.ps1') `
        @packageArguments
}
