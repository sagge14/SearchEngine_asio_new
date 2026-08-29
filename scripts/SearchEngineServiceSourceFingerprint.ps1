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
        'tools/config',
        'tools/token_issuer',
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
    if ($trackedFiles.Count -eq 0) {
        throw 'SearchEngineService source fingerprint has no tracked inputs.'
    }
    [Array]::Sort($trackedFiles, [StringComparer]::Ordinal)

    $fingerprintText = New-Object Text.StringBuilder
    foreach ($relativePath in $trackedFiles) {
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
        FileCount = $trackedFiles.Count
    }
}
