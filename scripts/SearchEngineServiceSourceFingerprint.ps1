#Requires -Version 5.1

function Get-SearchEngineServiceSourceFingerprint {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$ProjectRoot
    )

    $ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
    $pathSpecs = @(
        'CMakeLists.txt',
        'CMakePresets.json',
        'app-version.json',
        'cmake',
        'lib',
        'src',
        'tools/archive',
        'tools/auth',
        'tools/access_setup',
        'tools/config',
        'tools/token_issuer',
        'tutorials',
        'deployment/SearchEngineServicePortable',
        'scripts/AppVersion.ps1',
        'scripts/Assert-SearchEngineConfigAutoPadContract.ps1',
        'scripts/Build-SearchEngineServicePackage.ps1',
        'scripts/ConsoleScriptEncoding.ps1',
        'scripts/Ensure-ReleaseVersionBump.ps1',
        'scripts/Find-WorkspaceReleaseRoot.ps1',
        'scripts/New-SearchEngineServicePackage.ps1',
        'scripts/PostBuild-SearchEngineServicePackage.ps1',
        'scripts/SearchEngineServiceSourceFingerprint.ps1'
    )

    $trackedFiles = @(& git -c "safe.directory=$ProjectRoot" `
        -C $ProjectRoot ls-files -- @pathSpecs)
    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot enumerate SearchEngineService source fingerprint inputs.'
    }
    $trackedFiles = @($trackedFiles | Where-Object {
        -not [string]::IsNullOrWhiteSpace([string]$_)
    })

    # Tutorials are copied into the portable package even for a dirty candidate
    # build. Include every actual tutorial file in the fingerprint, including
    # untracked files, so a stand cannot reuse a package with stale instructions.
    $tutorialRoot = Join-Path $ProjectRoot 'tutorials'
    $tutorialFiles = @()
    if (Test-Path -LiteralPath $tutorialRoot -PathType Container) {
        $tutorialFiles = @(Get-ChildItem -LiteralPath $tutorialRoot `
            -Recurse -File -Force | ForEach-Object {
                $_.FullName.Substring($ProjectRoot.Length + 1).Replace('\', '/')
            })
    }

    $accessFiles = @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'tools\access_setup') -File | ForEach-Object { $_.FullName.Substring($ProjectRoot.Length + 1).Replace('\', '/') })
    $sourceFiles = @($trackedFiles + $tutorialFiles + $accessFiles + @('deployment/SearchEngineServicePortable/Setup-Access.bat') | Sort-Object -Unique)
    if ($sourceFiles.Count -eq 0) {
        throw 'SearchEngineService source fingerprint has no source inputs.'
    }
    [Array]::Sort($sourceFiles, [StringComparer]::Ordinal)

    $fingerprintText = New-Object Text.StringBuilder
    foreach ($relativePath in $sourceFiles) {
        $normalizedRelative = ([string]$relativePath).Replace('\', '/')
        $absolutePath = Join-Path $ProjectRoot `
            $normalizedRelative.Replace('/', '\')
        if (-not (Test-Path -LiteralPath $absolutePath -PathType Leaf)) {
            throw "SearchEngineService fingerprint input is missing: $normalizedRelative"
        }
        $fileHash = (Get-FileHash -LiteralPath $absolutePath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$fingerprintText.Append($normalizedRelative)
        [void]$fingerprintText.Append([char]0)
        [void]$fingerprintText.Append($fileHash)
        [void]$fingerprintText.Append("`n")
    }

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
            $fingerprintText.ToString()
        )
        $digest = $sha256.ComputeHash($bytes)
    }
    finally {
        $sha256.Dispose()
    }

    [pscustomobject]@{
        Algorithm = 'searchengine-service-source-v1'
        Value = (($digest | ForEach-Object { $_.ToString('x2') }) -join '')
        FileCount = $sourceFiles.Count
    }
}
