#Requires -Version 5.1
<#
.SYNOPSIS
  Build and publish a versioned synthetic SearchEngine archive stand.

.DESCRIPTION
  This is deliberately separate from Build-SearchEngineServicePackage.ps1.
  It builds only the Windows 7 x86 stand generator, selects the newest already
  built x86 SearchEngineService portable package, generates a v3 stand,
  verifies it, writes release metadata/checksums, and optionally publishes a
  ZIP to the dedicated SearchEngineArchiveE2EStand release channel.
#>
[CmdletBinding()]
param(
    [string]$ServicePackageDirectory,

    [ValidateRange(2000, 2099)]
    [int]$Year = (Get-Date).Year,

    [ValidateRange(10, 100)]
    [int]$RecordsPerMonth = 10,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$')]
    [string]$ServiceName = 'SearchEngineService-StandV3',

    [ValidateRange(1, 65535)]
    [int]$Port = 25027,

    [string]$OutputDirectory,
    [string]$RestoreRoot,
    [string]$CloudRoot,
    [string]$CloudReleaseId,
    [switch]$SkipCloudPublish,
    [switch]$SkipConfigure,
    [switch]$SkipVersionBump,
    [switch]$AllowDirtySource
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')

$productName = 'SearchEngineArchiveE2EStand'
$configurePreset = 'windows7-x86-archive-e2e-stand'
$buildPreset = 'windows7-x86-archive-e2e-stand-release'
$packageLeaf = "$ServiceName-$Year"

function Resolve-AbsolutePath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'An empty path is not allowed.'
    }
    if (-not [IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path $BasePath $Path
    }
    return [IO.Path]::GetFullPath($Path)
}

function Test-PathInside([string]$Path, [string]$Root) {
    $resolvedPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $resolvedPath.Equals(
        $resolvedRoot,
        [StringComparison]::OrdinalIgnoreCase
    ) -or $resolvedPath.StartsWith(
        $resolvedRoot + '\',
        [StringComparison]::OrdinalIgnoreCase
    )
}

function Resolve-RequiredFile([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-ServicePackage([string]$PackageRoot) {
    $marker = Join-Path $PackageRoot '.searchengine-portable-package'
    $manifestPath = Join-Path $PackageRoot 'package-manifest.json'
    Resolve-RequiredFile $marker 'SearchEngineService package marker' | Out-Null
    Resolve-RequiredFile $manifestPath 'SearchEngineService package manifest' | Out-Null

    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ([int]$manifest.formatVersion -ne 1 -or
        [string]$manifest.product -ne 'SearchEngineService') {
        throw "Not a SearchEngineService portable package: $PackageRoot"
    }
    $packageArchitecture = [string]$manifest.architecture
    $minimumWindowsVersion = [string]$manifest.minimumWindowsVersion
    if ($packageArchitecture -notin @('x86', 'x86-modern') -or
        [string]::IsNullOrWhiteSpace($minimumWindowsVersion)) {
        throw (
            'The stand release requires an x86 SearchEngineService package; ' +
            "got architecture='$packageArchitecture', " +
            "minimumWindowsVersion='$minimumWindowsVersion'."
        )
    }

    foreach ($entry in @($manifest.files)) {
        $relative = ([string]$entry.path).Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or
            [IO.Path]::IsPathRooted($relative)) {
            throw "Unsafe package manifest path: '$relative'."
        }
        $candidate = [IO.Path]::GetFullPath((Join-Path $PackageRoot $relative))
        if (-not (Test-PathInside $candidate $PackageRoot)) {
            throw "Package manifest path escapes the package: '$relative'."
        }
        Resolve-RequiredFile $candidate "Protected package file '$relative'" |
            Out-Null
        $item = Get-Item -LiteralPath $candidate
        if ([Int64]$item.Length -ne [Int64]$entry.size) {
            throw "Package file size mismatch: '$relative'."
        }
        $actualHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
        if ($actualHash -ne [string]$entry.sha256) {
            throw "Package file SHA-256 mismatch: '$relative'."
        }
    }

    foreach ($required in @(
        'app\SearchEngine.exe',
        'data\Settings.json',
        'tools\SearchEngineConfig.exe',
        'tools\SearchEngineArchive.exe',
        'prerequisites\vc_redist.x86.exe'
    )) {
        Resolve-RequiredFile (Join-Path $PackageRoot $required) `
            "Required stand source '$required'" | Out-Null
    }
    return $manifest
}

function Assert-FreshYearSettings([string]$PackageRoot, [int]$ReleaseYear) {
    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'SearchEngineArchiveE2EStand-settings-' +
        [Guid]::NewGuid().ToString('N')
    )
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        $expectedSettings = Join-Path $temporaryRoot 'Settings.json'
        & (Join-Path $PSScriptRoot 'Prepare-YearBasedReleaseSettings.ps1') `
            -TemplatePath (Join-Path $projectRoot (
                'deployment\SearchEngineServicePortable\' +
                'source-data\Settings.json'
            )) `
            -OutputPath $expectedSettings `
            -AllowedOutputRoot $temporaryRoot `
            -Year $ReleaseYear `
            -ConfigToolPath (Join-Path $PackageRoot (
                'tools\SearchEngineConfig.exe'
            )) | Out-Null

        $packageSettings = Join-Path $PackageRoot 'data\Settings.json'
        $expectedHash = (Get-FileHash -LiteralPath $expectedSettings `
            -Algorithm SHA256).Hash
        $packageHash = (Get-FileHash -LiteralPath $packageSettings `
            -Algorithm SHA256).Hash
        if ($packageHash -ne $expectedHash) {
            throw (
                'The service package data\Settings.json is edited, stale, ' +
                "or was generated for another year. Build a fresh Windows 7 " +
                "x86 package with -Year $ReleaseYear before publishing a stand."
            )
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    }
}

function Get-PlannedVersion([string]$CurrentVersion, [bool]$BumpPatch) {
    if (-not $BumpPatch) {
        return $CurrentVersion
    }
    $parts = @($CurrentVersion.Split('.'))
    if ($parts.Count -ne 3) {
        throw "Invalid current stand tool version: $CurrentVersion"
    }
    return '{0}.{1}.{2}' -f [int]$parts[0], [int]$parts[1], ([int]$parts[2] + 1)
}

function Get-ServicePackageSearchRoots([string]$ProjectRoot) {
    $candidates = New-Object System.Collections.Generic.List[string]
    $candidates.Add((Join-Path $ProjectRoot 'out\package'))

    $commonDirectory = & git -c "safe.directory=$ProjectRoot" `
        -C $ProjectRoot rev-parse --git-common-dir
    if ($LASTEXITCODE -eq 0 -and
        -not [string]::IsNullOrWhiteSpace([string]$commonDirectory)) {
        $commonDirectory = ([string]$commonDirectory).Trim()
        if (-not [IO.Path]::IsPathRooted($commonDirectory)) {
            $commonDirectory = Join-Path $ProjectRoot $commonDirectory
        }
        $commonDirectory = [IO.Path]::GetFullPath($commonDirectory)
        $primaryWorktree = Split-Path -Parent $commonDirectory
        $candidates.Add((Join-Path $primaryWorktree 'out\package'))
    }

    $seen = @{}
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            continue
        }
        $resolved = (Resolve-Path -LiteralPath $candidate).Path
        $key = $resolved.ToLowerInvariant()
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            $resolved
        }
    }
}

function Find-LatestServicePackage([string[]]$SearchRoots) {
    $packages = New-Object System.Collections.Generic.List[object]
    $seenDirectories = @{}
    foreach ($root in @($SearchRoots)) {
        foreach ($manifestFile in @(Get-ChildItem -LiteralPath $root `
                -Filter 'package-manifest.json' -File -Recurse)) {
            $packageDirectory = $manifestFile.Directory.FullName
            $directoryKey = $packageDirectory.ToLowerInvariant()
            if ($seenDirectories.ContainsKey($directoryKey) -or
                -not (Test-Path -LiteralPath (Join-Path $packageDirectory (
                        '.searchengine-portable-package')) -PathType Leaf)) {
                continue
            }
            $seenDirectories[$directoryKey] = $true
            try {
                $manifest = Get-Content -LiteralPath $manifestFile.FullName `
                    -Raw -Encoding UTF8 | ConvertFrom-Json
                $architecture = [string]$manifest.architecture
                if ([string]$manifest.product -ne 'SearchEngineService' -or
                    $architecture -notin @('x86', 'x86-modern')) {
                    continue
                }
                $version = [Version]([string]$manifest.applicationVersion)
                $packages.Add([pscustomobject]@{
                    Directory = $packageDirectory
                    Version = $version
                    ArchitectureRank = if ($architecture -eq 'x86-modern') {
                        1
                    } else {
                        0
                    }
                    CreatedUtc = [string]$manifest.createdUtc
                })
            }
            catch {
                continue
            }
        }
    }
    if ($packages.Count -eq 0) {
        return $null
    }
    return @($packages | Sort-Object `
        @{ Expression = { $_.Version }; Descending = $true }, `
        @{ Expression = { $_.ArchitectureRank }; Descending = $true }, `
        @{ Expression = { $_.CreatedUtc }; Descending = $true })[0]
}

$servicePackageSearchRoots = @(Get-ServicePackageSearchRoots $projectRoot)
$latestServicePackage = $null
if ($servicePackageSearchRoots.Count -gt 0) {
    $latestServicePackage = Find-LatestServicePackage `
        $servicePackageSearchRoots
}
if ([string]::IsNullOrWhiteSpace($ServicePackageDirectory)) {
    if ($null -eq $latestServicePackage) {
        throw 'The latest built x86 SearchEngineService package was not found.'
    }
    $ServicePackageDirectory = $latestServicePackage.Directory
    Write-Host (
        'Selected latest SearchEngineService package automatically: ' +
        "$ServicePackageDirectory ($($latestServicePackage.Version))"
    )
}
$ServicePackageDirectory = Resolve-AbsolutePath `
    $ServicePackageDirectory $projectRoot
if (-not (Test-Path -LiteralPath $ServicePackageDirectory -PathType Container)) {
    throw "Service package directory was not found: $ServicePackageDirectory"
}
$ServicePackageDirectory = (Resolve-Path -LiteralPath $ServicePackageDirectory).Path
$servicePackageManifest = Assert-ServicePackage $ServicePackageDirectory
$selectedServicePackageVersion = [Version](
    [string]$servicePackageManifest.applicationVersion
)
if ($null -ne $latestServicePackage -and
    $selectedServicePackageVersion -lt $latestServicePackage.Version) {
    throw (
        "Selected SearchEngineService package '$selectedServicePackageVersion' " +
        "is older than the latest built package '$($latestServicePackage.Version)': " +
        $latestServicePackage.Directory
    )
}
Assert-FreshYearSettings $ServicePackageDirectory $Year

$currentVersion = Get-SearchEngineAppVersionNames `
    -ProjectRoot $projectRoot `
    -ProductName $productName
$plannedVersion = Get-PlannedVersion `
    -CurrentVersion $currentVersion.Version `
    -BumpPatch:(-not $SkipVersionBump)
if ([string]::IsNullOrWhiteSpace($CloudReleaseId)) {
    $CloudReleaseId = $plannedVersion
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Get-SearchEnginePackageOutputDirectory `
        -ProjectRoot $projectRoot `
        -ReleaseId $CloudReleaseId `
        -PackageLeaf $packageLeaf
}
$OutputDirectory = Resolve-AbsolutePath $OutputDirectory $projectRoot
if ([IO.Path]::GetFileName($OutputDirectory.TrimEnd('\')) -cne $packageLeaf) {
    throw "Output directory leaf must be '$packageLeaf': $OutputDirectory"
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Stand release output already exists and was preserved: $OutputDirectory"
}
$outputVolumeRoot = [IO.Path]::GetPathRoot($OutputDirectory)
if ($OutputDirectory.TrimEnd('\').Equals(
    $outputVolumeRoot.TrimEnd('\'),
    [StringComparison]::OrdinalIgnoreCase
) -or $OutputDirectory.Equals(
    $projectRoot,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw "Unsafe stand release output directory: $OutputDirectory"
}
if ((Test-PathInside $OutputDirectory $ServicePackageDirectory) -or
    (Test-PathInside $ServicePackageDirectory $OutputDirectory)) {
    throw 'Stand output and source service package must not overlap.'
}

if ([string]::IsNullOrWhiteSpace($RestoreRoot)) {
    $RestoreRoot = Join-Path (Split-Path -Parent $OutputDirectory) `
        "restored-$packageLeaf"
}
$RestoreRoot = Resolve-AbsolutePath $RestoreRoot $projectRoot
if ((Test-PathInside $RestoreRoot $OutputDirectory) -or
    (Test-PathInside $OutputDirectory $RestoreRoot)) {
    throw 'Restore root must be outside the stand release directory.'
}

$lockScript = Resolve-SearchEngineReleaseScript `
    -ProjectRoot $projectRoot `
    -ScriptName 'Enter-WorkspaceReleaseLock.ps1'
$releaseLock = & $lockScript -ProjectRoot $projectRoot
try {
    $sourceCommit = & git -c "safe.directory=$projectRoot" `
        -C $projectRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($sourceCommit)) {
        throw 'Unable to resolve source Git commit for stand release metadata.'
    }
    $sourceCommit = ([string]$sourceCommit).Trim()
    $sourceStatus = @(& git -c "safe.directory=$projectRoot" `
        -C $projectRoot status --porcelain --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to inspect source Git status for stand release.'
    }
    $sourceDirtyBeforeRelease = $sourceStatus.Count -gt 0
    if ($sourceDirtyBeforeRelease -and -not $AllowDirtySource) {
        throw (
            'Stand release requires a clean Git worktree so sourceCommit is ' +
            'reproducible. Commit/stash the changes or use -AllowDirtySource ' +
            'only for a disposable local smoke test.'
        )
    }

    $versionInfo = Sync-SearchEngineAppVersion `
        -ProjectRoot $projectRoot `
        -ProductName $productName `
        -BumpPatch:(-not $SkipVersionBump)
    if ($versionInfo.Version -ne $plannedVersion) {
        throw (
            "Stand version changed during release: planned '$plannedVersion', " +
            "actual '$($versionInfo.Version)'."
        )
    }

    Push-Location $projectRoot
    try {
        if (-not $SkipConfigure) {
            & cmake --preset $configurePreset
            if ($LASTEXITCODE -ne 0) {
                throw "CMake configure '$configurePreset' failed with exit code $LASTEXITCODE."
            }
        }
        & cmake --build --preset $buildPreset `
            --target SearchEngineArchiveE2EStand -- /m
        if ($LASTEXITCODE -ne 0) {
            throw (
                'SearchEngineArchiveE2EStand Windows 7 x86 Release build failed ' +
                "with exit code $LASTEXITCODE."
            )
        }
    }
    finally {
        Pop-Location
    }

    $helperPath = Resolve-RequiredFile `
        (Join-Path $projectRoot (
            'out\build\windows7-x86-archive-e2e-stand\Release\' +
            'SearchEngineArchiveE2EStand.exe'
        )) `
        'SearchEngineArchiveE2EStand Release binary'
    Assert-PeMatchesAppVersion `
        -BinaryPath $helperPath `
        -ExpectedProductVersion $versionInfo.Version `
        -ExpectedFileVersion $versionInfo.FileVersion `
        -ExpectedProductName $productName `
        -ExpectedOriginalFilename 'SearchEngineArchiveE2EStand.exe'

    $generatorArguments = @(
        'generate-service-archive',
        '--root', $OutputDirectory,
        '--deployment-root', $OutputDirectory,
        '--restore-root', $RestoreRoot,
        '--program-template', (Join-Path $ServicePackageDirectory 'app'),
        '--preparer-template', $helperPath,
        '--installer-template', $ServicePackageDirectory,
        '--settings-template', (Join-Path $ServicePackageDirectory 'data\Settings.json'),
        '--service-name', $ServiceName,
        '--port', [string]$Port,
        '--year', [string]$Year,
        '--records', [string]$RecordsPerMonth
    )
    & $helperPath @generatorArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Stand generation failed with exit code $LASTEXITCODE."
    }
    & $helperPath verify --root $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Generated stand verification failed with exit code $LASTEXITCODE."
    }

    $standManifest = Get-Content `
        -LiteralPath (Join-Path $OutputDirectory 'stand-manifest.json') `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $releaseManifest = [ordered]@{
        formatVersion = 1
        product = $productName
        applicationVersion = $versionInfo.Version
        fileVersion = $versionInfo.FileVersion
        releaseId = $CloudReleaseId
        architecture = [string]$servicePackageManifest.architecture
        minimumWindowsVersion = [string]$servicePackageManifest.minimumWindowsVersion
        servicePackageVersion = [string]$servicePackageManifest.applicationVersion
        serviceName = $ServiceName
        year = $Year
        recordsPerMonth = $RecordsPerMonth
        telegramRows = [int]$standManifest.f12.rows
        createdUtc = (Get-Date).ToUniversalTime().ToString('o')
        sourceCommit = $sourceCommit
        sourceDirtyBeforeRelease = $sourceDirtyBeforeRelease
    }
    [IO.File]::WriteAllText(
        (Join-Path $OutputDirectory 'stand-release.json'),
        (($releaseManifest | ConvertTo-Json -Depth 5) + "`r`n"),
        (New-Object Text.UTF8Encoding($false))
    )

    $checksumEntries = @(
        Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
            Where-Object { $_.Name -ne 'stand-release-checksums.sha256' } |
            Sort-Object FullName |
            ForEach-Object {
                $relative = $_.FullName.Substring($OutputDirectory.Length + 1)
                '{0}  {1}' -f `
                    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), `
                    $relative
            }
    )
    [IO.File]::WriteAllLines(
        (Join-Path $OutputDirectory 'stand-release-checksums.sha256'),
        $checksumEntries,
        (New-Object Text.UTF8Encoding($true))
    )

    & $helperPath verify --root $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Released stand verification failed with exit code $LASTEXITCODE."
    }

    $cloudHelper = & (Join-Path $PSScriptRoot 'Find-WorkspaceReleaseRoot.ps1') `
        -Name 'Publish-ReleasePackageIfConfigured.ps1' `
        -StartPath $projectRoot `
        -Optional
    $cloudPackageRoot = $null
    try {
        if ($null -ne $cloudHelper) {
            $cloudPackageDirectory = $OutputDirectory
            if (-not $SkipCloudPublish) {
                # The deploy tool deliberately requires the portable stand to
                # retain its <service-name>-<year> directory name. The shared
                # publisher archives the contents of PackageDirectory, so give
                # it a temporary parent containing exactly that stand folder.
                $cloudPackageRoot = Join-Path ([IO.Path]::GetTempPath()) (
                    'SearchEngineArchiveE2EStand-publish-' +
                    [Guid]::NewGuid().ToString('N')
                )
                New-Item -ItemType Directory -Path $cloudPackageRoot |
                    Out-Null
                Copy-Item -LiteralPath $OutputDirectory `
                    -Destination $cloudPackageRoot -Recurse -Force

                $cloudPackageDirectory = Join-Path $cloudPackageRoot $packageLeaf
                if (-not (Test-Path -LiteralPath $cloudPackageDirectory `
                        -PathType Container) -or
                    -not (Test-Path -LiteralPath (Join-Path (
                            $cloudPackageDirectory
                        ) '.searchengine-archive-e2e-stand') -PathType Leaf)) {
                    throw 'Failed to stage the named portable stand directory for ZIP publishing.'
                }
                $cloudPackageDirectory = $cloudPackageRoot
            }

            $cloudArguments = @{
                PackageDirectory = $cloudPackageDirectory
                ProductName = $productName
                ZipName = "$productName-$Year-$CloudReleaseId.zip"
                ReleaseId = $CloudReleaseId
                StartPath = $projectRoot
                SkipCloudPublish = $SkipCloudPublish
                AllowSensitive = $true
            }
            if (-not [string]::IsNullOrWhiteSpace($CloudRoot)) {
                $cloudArguments.CloudRoot = $CloudRoot
            }
            & $cloudHelper @cloudArguments | Out-Null
        }
        else {
            Write-Host 'Cloud publish helper not found; local stand release only.'
        }
    }
    finally {
        if ($null -ne $cloudPackageRoot -and
            (Test-Path -LiteralPath $cloudPackageRoot -PathType Container)) {
            Remove-Item -LiteralPath $cloudPackageRoot -Recurse -Force
        }
    }

    $releaseSize = (Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
        Measure-Object -Property Length -Sum).Sum
    Write-Host 'SearchEngine archive E2E stand release created successfully.'
    Write-Host "Stand: $OutputDirectory"
    Write-Host "Stand tool version: $($versionInfo.Version)"
    Write-Host "Source service package version: $($servicePackageManifest.applicationVersion)"
    Write-Host "Year: $Year"
    Write-Host "Size: $([Math]::Round($releaseSize / 1MB, 1)) MB"

    [pscustomobject]@{
        OutputDirectory = $OutputDirectory
        Version = $versionInfo.Version
        ReleaseId = $CloudReleaseId
        Year = $Year
        Size = $releaseSize
    }
}
finally {
    if ($null -ne $releaseLock -and $releaseLock.Acquired) {
        try {
            $releaseLock.Mutex.ReleaseMutex()
        }
        finally {
            $releaseLock.Mutex.Dispose()
        }
    }
}
