# Static release-boundary tests. No build, package, service, or cloud write.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptsRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent $scriptsRoot
$releaseScript = Join-Path $scriptsRoot `
    'Build-SearchEngineArchiveE2EStandRelease.ps1'
$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if ($Condition) {
        $script:passed++
        Write-Host "PASS: $Message"
    }
    else {
        $script:failures.Add($Message) | Out-Null
        Write-Host "FAIL: $Message"
    }
}

function Assert-Equal($Expected, $Actual, [string]$Message) {
    Assert-True ($Expected -ceq $Actual) (
        "$Message (expected='$Expected', actual='$Actual')"
    )
}

Write-Host '=== Archive E2E stand release boundary tests ==='

$tokens = $null
$parseErrors = $null
[Management.Automation.Language.Parser]::ParseFile(
    $releaseScript,
    [ref]$tokens,
    [ref]$parseErrors
) | Out-Null
Assert-Equal 0 @($parseErrors).Count 'stand release PowerShell parses without errors'

$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') `
    -Raw
Assert-True ($cmakeText -match (
    'option\s*\(\s*BUILD_ARCHIVE_E2E_STAND_TOOL[\s\S]*?OFF\s*\)'
)) 'stand generator option defaults to OFF'
Assert-True ($cmakeText -match (
    'if\s*\(BUILD_ARCHIVE_E2E_STAND_TOOL\)[\s\S]*?' +
    'add_executable\s*\(\s*SearchEngineArchiveE2EStand'
)) 'stand executable exists only behind its explicit option'
Assert-True ($cmakeText -match (
    'PRODUCT_NAME\s+"SearchEngineArchiveE2EStand"'
)) 'stand executable has independent VERSIONINFO product name'

$presets = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakePresets.json') `
    -Raw | ConvertFrom-Json
$standConfigure = @($presets.configurePresets | Where-Object {
    $_.name -eq 'windows7-x86-archive-e2e-stand'
})
Assert-Equal 1 $standConfigure.Count 'dedicated stand configure preset exists'
if ($standConfigure.Count -eq 1) {
    $cache = $standConfigure[0].cacheVariables
    Assert-Equal 'ON' $cache.BUILD_ARCHIVE_E2E_STAND_TOOL `
        'dedicated preset enables stand generator'
    foreach ($name in @(
        'BUILD_SEARCHENGINE',
        'BUILD_CONFIG_TOOL',
        'BUILD_ARCHIVE_TOOL',
        'BUILD_ZAGEDITOR',
        'BUILD_BACKUP_SERVICE',
        'BUILD_BACKUP_RESTORE',
        'BUILD_AUTH_DB_TOOL',
        'BUILD_TOKEN_ISSUER',
        'SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD'
    )) {
        Assert-Equal 'OFF' ([string]$cache.$name) `
            "dedicated preset disables $name"
    }
}
$standBuild = @($presets.buildPresets | Where-Object {
    $_.name -eq 'windows7-x86-archive-e2e-stand-release'
})
Assert-Equal 1 $standBuild.Count 'dedicated stand Release build preset exists'

$serviceBuildText = Get-Content -LiteralPath (
    Join-Path $scriptsRoot 'Build-SearchEngineServicePackage.ps1'
) -Raw
$servicePackageText = Get-Content -LiteralPath (
    Join-Path $scriptsRoot 'New-SearchEngineServicePackage.ps1'
) -Raw
Assert-True ($serviceBuildText -notmatch 'SearchEngineArchiveE2EStand') `
    'ordinary SearchEngine build does not build the stand generator'
Assert-True ($servicePackageText -notmatch 'SearchEngineArchiveE2EStand') `
    'ordinary SearchEngine package does not include the stand generator'

$versionManifest = Get-Content -LiteralPath (
    Join-Path $projectRoot 'app-version.SearchEngineArchiveE2EStand.json'
) -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-Equal 1 ([int]$versionManifest.formatVersion) `
    'stand version manifest uses format 1'
Assert-Equal 'SearchEngineArchiveE2EStand' ([string]$versionManifest.productName) `
    'stand version manifest has independent product name'

$releaseText = Get-Content -LiteralPath $releaseScript -Raw
Assert-True ($releaseText -match "architecture\s+-ne\s+'x86'") `
    'release rejects a non-Windows-7-x86 service package'
Assert-True ($releaseText -match "minimumWindowsVersion\s+-ne\s+'6\.1'") `
    'release requires Windows 7 package compatibility'
Assert-True ($releaseText -match 'ProductName\s*=\s*\$productName') `
    'stand uses a dedicated cloud product channel'
Assert-True ($releaseText -match 'AllowSensitive\s*=\s*\$true') `
    'synthetic SQLite stand is explicitly allowed by its own publisher'
Assert-True ($releaseText -match (
    'stand-release-checksums\.sha256''[\s\S]*?Text\.UTF8Encoding\(\$true\)'
)) 'release checksums preserve Cyrillic paths as UTF-8 BOM'
Assert-True ($releaseText -notmatch (
    'stand-release-checksums\.sha256''[\s\S]*?Text\.Encoding\]::ASCII'
)) 'release checksums never replace Cyrillic path characters with question marks'
Assert-True ($releaseText -match (
    'sourceDirtyBeforeRelease[\s\S]*?-not \$AllowDirtySource'
)) 'canonical stand release rejects a dirty source worktree'
Assert-True ($releaseText -match (
    'Prepare-YearBasedReleaseSettings\.ps1[\s\S]*?packageHash[\s\S]*?expectedHash'
)) 'release rejects edited or stale mutable package Settings'

if ($script:failures.Count -gt 0) {
    throw (
        "Archive E2E stand release tests failed: $($script:failures.Count); " +
        "passed: $script:passed."
    )
}

Write-Host "All archive E2E stand release tests passed: $script:passed."
