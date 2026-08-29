[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'x86-modern')]
    [string]$Architecture = 'x64',
    [string]$BuildDirectory,
    [string]$ConfigPath,
    [string]$DictPath,
    [string]$OutputDirectory,
    [string]$VCRedistPath,
    [string]$CloudRoot,
    [string]$CloudReleaseId,
    [switch]$SkipCloudPublish
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')
$templateRoot = Join-Path $projectRoot 'deployment\ZagEditorPortable'
$sourceDataRoot = Join-Path $templateRoot 'source-data'
$packageMarkerName = '.zageditor-portable-package'
$productName = 'ZagEditor'
$versionInfo = Get-SearchEngineAppVersionNames `
    -ProjectRoot $projectRoot `
    -ProductName $productName
if ([string]::IsNullOrWhiteSpace($CloudReleaseId)) {
    $CloudReleaseId = $versionInfo.ReleaseId
}

if ($Architecture -eq 'x86') {
    $packageName = 'ZagEditor-x86-Windows7'
    $minimumWindowsVersion = '6.1'
    $minimumWindowsLabel = 'Windows 7 SP1'
    $expectedMachine = 0x014c
    $buildPresetDirectory = 'windows7-x86'
    $visualStudioVersionRange = '[16.0,17.0)'
    $expectedToolset = 'v142'
} elseif ($Architecture -eq 'x86-modern') {
    $packageName = 'ZagEditor-x86'
    $minimumWindowsVersion = '10.0'
    $minimumWindowsLabel = 'Windows 10 / Windows Server 2016'
    $expectedMachine = 0x014c
    $buildPresetDirectory = 'windows-x86'
    $visualStudioVersionRange = '[17.0,18.0)'
    $expectedToolset = 'v143'
} else {
    $packageName = 'ZagEditor-x64'
    $minimumWindowsVersion = '10.0'
    $minimumWindowsLabel = 'Windows 10 / Windows Server 2016'
    $expectedMachine = 0x8664
    $buildPresetDirectory = 'windows-x64'
    $visualStudioVersionRange = '[17.0,18.0)'
    $expectedToolset = 'v143'
}
$vcRedistArch = if ($Architecture -eq 'x64') { 'x64' } else { 'x86' }
$vcRedistName = "vc_redist.$vcRedistArch.exe"

function Resolve-AbsolutePath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'An empty path is not allowed.'
    }
    if (-not [IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path $BasePath $Path
    }
    return [IO.Path]::GetFullPath($Path)
}

function Resolve-RequiredFile([string]$Path, [string]$Description) {
    $resolved = Resolve-AbsolutePath $Path $projectRoot
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Description was not found: $resolved"
    }
    return (Resolve-Path -LiteralPath $resolved).Path
}

function Find-VCRedist([string]$FileName, [string]$VersionRange) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found. Pass -VCRedistPath explicitly.'
    }

    $installationPath = & $vswhere -latest -products * `
        -version $VersionRange -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
        throw "Visual Studio $VersionRange was not found for $Architecture packaging."
    }

    $redistRoot = Join-Path ([string]$installationPath) 'VC\Redist\MSVC'
    $candidate = Get-ChildItem -LiteralPath $redistRoot `
        -Filter $FileName -Recurse -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $candidate) {
        throw "$FileName was not found below $redistRoot"
    }
    return $candidate.FullName
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a PE executable: $Path"
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Invalid PE signature: $Path"
        }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot `
        "out\build\$buildPresetDirectory\Release"
}
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $sourceDataRoot 'zag_editor.ini'
}
if ([string]::IsNullOrWhiteSpace($DictPath)) {
    $DictPath = Join-Path $sourceDataRoot 'EXPORT.INI'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Get-SearchEnginePackageOutputDirectory `
        -ProjectRoot $projectRoot `
        -ReleaseId $CloudReleaseId `
        -PackageLeaf $packageName
}
if ([string]::IsNullOrWhiteSpace($VCRedistPath)) {
    $VCRedistPath = Find-VCRedist $vcRedistName $visualStudioVersionRange
}

$BuildDirectory = Resolve-AbsolutePath $BuildDirectory $projectRoot
if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) {
    throw "Release build directory was not found: $BuildDirectory"
}
if ([IO.Path]::GetFileName($BuildDirectory) -ne 'Release') {
    throw "Only a Release directory can be packaged: $BuildDirectory"
}

$binaryPath = Resolve-RequiredFile `
    (Join-Path $BuildDirectory 'ZagEditor.exe') 'ZagEditor Release binary'
Assert-PeMatchesAppVersion `
    -BinaryPath $binaryPath `
    -ExpectedProductVersion $versionInfo.Version `
    -ExpectedFileVersion $versionInfo.FileVersion `
    -ExpectedProductName $productName `
    -ExpectedOriginalFilename 'ZagEditor.exe'
$ConfigPath = Resolve-RequiredFile $ConfigPath 'Portable zag_editor.ini'
$DictPath = Resolve-RequiredFile $DictPath 'Portable EXPORT.INI sample'
$VCRedistPath = Resolve-RequiredFile $VCRedistPath `
    "Microsoft Visual C++ Redistributable $vcRedistArch"

$machine = Get-PeMachine $binaryPath
if ($machine -ne $expectedMachine) {
    throw ('ZagEditor.exe architecture mismatch: expected 0x{0:X4}, got 0x{1:X4}.' `
        -f $expectedMachine, $machine)
}

$cachePath = Join-Path (Split-Path -Parent $BuildDirectory) 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache was not found for the selected build: $cachePath"
}
$cacheText = [IO.File]::ReadAllText($cachePath)
if ($Architecture -eq 'x86' -and
    ($cacheText -notmatch 'CMAKE_GENERATOR:INTERNAL=Visual Studio 16 2019' -or
     $cacheText -notmatch 'SEARCHENGINE_WINDOWS_TARGET_VERSION:STRING=0x0601')) {
    throw 'The x86 package must be built by VS2019 v142 with Windows target 0x0601.'
}
if ($cacheText -notmatch 'BUILD_ZAGEDITOR:BOOL=ON') {
    throw "The selected build does not have BUILD_ZAGEDITOR enabled: $cachePath"
}

$signature = Get-AuthenticodeSignature -LiteralPath $VCRedistPath
if ($signature.Status -ne 'Valid' -or
    $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "VC++ Redistributable does not have a valid Microsoft signature: $VCRedistPath"
}
if ($Architecture -eq 'x86' -and
    (Get-Item -LiteralPath $VCRedistPath).VersionInfo.FileVersion -notlike '14.29.*') {
    throw 'The Windows 7 package requires the VS2019 VC++ Runtime 14.29 x86.'
}

$OutputDirectory = Resolve-AbsolutePath $OutputDirectory $projectRoot
$outputRoot = [IO.Path]::GetPathRoot($OutputDirectory)
if ($OutputDirectory.TrimEnd('\').Equals(
    $outputRoot.TrimEnd('\'),
    [StringComparison]::OrdinalIgnoreCase
) -or $OutputDirectory.Equals(
    $projectRoot,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw "Unsafe package output directory: $OutputDirectory"
}

$outputParent = Split-Path -Parent $OutputDirectory
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
$stagingDirectory = Join-Path $outputParent `
    (".$packageName.staging-" + [Guid]::NewGuid().ToString('N'))
$previousDirectory = $null
New-Item -ItemType Directory -Path $stagingDirectory | Out-Null

try {
    foreach ($directory in @('app', 'data', 'prerequisites')) {
        New-Item -ItemType Directory `
            -Path (Join-Path $stagingDirectory $directory) | Out-Null
    }

    Copy-Item -LiteralPath $binaryPath `
        -Destination (Join-Path $stagingDirectory 'app\ZagEditor.exe')
    Copy-Item -LiteralPath $ConfigPath `
        -Destination (Join-Path $stagingDirectory 'data\zag_editor.ini')
    Copy-Item -LiteralPath $DictPath `
        -Destination (Join-Path $stagingDirectory 'data\EXPORT.INI')

    foreach ($batchName in @('Run-ZagEditor.bat', 'Verify-Package.bat')) {
        Copy-Item `
            -LiteralPath (Join-Path $templateRoot $batchName) `
            -Destination (Join-Path $stagingDirectory $batchName)
    }

    $text = Get-Content -LiteralPath (Join-Path $templateRoot 'README.txt') `
        -Raw -Encoding UTF8
    $textTokens = @{
        '{{ARCHITECTURE}}' = $Architecture
        '{{MINIMUM_WINDOWS}}' = $minimumWindowsLabel
        '{{PACKAGE_NAME}}' = $packageName
        '{{VC_REDIST_FILE}}' = $vcRedistName
    }
    foreach ($token in $textTokens.Keys) {
        $text = $text.Replace($token, $textTokens[$token])
    }
    [IO.File]::WriteAllText(
        (Join-Path $stagingDirectory 'README.txt'),
        $text,
        (New-Object Text.UTF8Encoding($true))
    )

    Copy-Item -LiteralPath $VCRedistPath -Destination `
        (Join-Path $stagingDirectory ('prerequisites\' + $vcRedistName))

    New-Item -ItemType Directory `
        -Path (Join-Path $stagingDirectory 'data\logs') | Out-Null

    $protectedFiles = @()
    foreach ($relativeRoot in @('app', 'prerequisites')) {
        $absoluteRoot = Join-Path $stagingDirectory $relativeRoot
        $protectedFiles += Get-ChildItem -LiteralPath $absoluteRoot `
            -Recurse -File
    }
    foreach ($name in @('Run-ZagEditor.bat', 'Verify-Package.bat')) {
        $protectedFiles += Get-Item -LiteralPath `
            (Join-Path $stagingDirectory $name)
    }

    $manifestEntries = @($protectedFiles | Sort-Object FullName | ForEach-Object {
        $relativePath = $_.FullName.Substring($stagingDirectory.Length + 1)
        [ordered]@{
            path = $relativePath.Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            size = $_.Length
        }
    })
    if ($manifestEntries.path -contains 'data/zag_editor.ini' -or
        $manifestEntries.path -contains 'data/EXPORT.INI') {
        throw 'Mutable data templates must not be checksum-protected.'
    }
    $checksumLines = @($manifestEntries | ForEach-Object {
        '{0}  {1}' -f `
            ([string]$_.sha256).ToLowerInvariant(),
            ([string]$_.path).Replace('/', '\')
    })
    [IO.File]::WriteAllLines(
        (Join-Path $stagingDirectory 'package-checksums.sha256'),
        $checksumLines,
        [Text.Encoding]::ASCII
    )
    $manifest = [ordered]@{
        formatVersion = 1
        product = 'ZagEditor'
        architecture = $Architecture
        applicationVersion = $versionInfo.Version
        fileVersion = $versionInfo.FileVersion
        releaseId = $CloudReleaseId
        minimumWindowsVersion = $minimumWindowsVersion
        toolset = $expectedToolset
        vcRuntimeFile = $vcRedistName
        configSource = $ConfigPath
        createdUtc = (Get-Date).ToUniversalTime().ToString('o')
        files = $manifestEntries
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText(
        (Join-Path $stagingDirectory 'package-manifest.json'),
        $manifestJson,
        (New-Object Text.UTF8Encoding($false))
    )
    [IO.File]::WriteAllText(
        (Join-Path $stagingDirectory $packageMarkerName),
        "Generated by New-ZagEditorPackage.ps1`r`n",
        (New-Object Text.UTF8Encoding($false))
    )

    if (Test-Path -LiteralPath $OutputDirectory) {
        $existingMarker = Join-Path $OutputDirectory $packageMarkerName
        if (-not (Test-Path -LiteralPath $existingMarker -PathType Leaf)) {
            throw "Existing output has no package marker and was preserved: $OutputDirectory"
        }

        $previousDirectory = Join-Path $outputParent `
            (".$packageName.previous-" + [Guid]::NewGuid().ToString('N'))
        Rename-Item -LiteralPath $OutputDirectory `
            -NewName (Split-Path -Leaf $previousDirectory) -ErrorAction Stop
    }

    try {
        Move-Item -LiteralPath $stagingDirectory -Destination $OutputDirectory `
            -ErrorAction Stop
    } catch {
        if ($previousDirectory -and
            (Test-Path -LiteralPath $previousDirectory) -and
            -not (Test-Path -LiteralPath $OutputDirectory)) {
            Rename-Item -LiteralPath $previousDirectory `
                -NewName (Split-Path -Leaf $OutputDirectory) -ErrorAction Stop
        }
        throw
    }

    if ($previousDirectory -and (Test-Path -LiteralPath $previousDirectory)) {
        try {
            Remove-Item -LiteralPath $previousDirectory -Recurse -Force `
                -ErrorAction Stop
        } catch {
            Write-Warning (
                'The new package is ready, but the previous package could not ' +
                "be removed: $previousDirectory"
            )
        }
    }
} finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}

$packageSize = (Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
Write-Host 'Portable package created successfully.'
Write-Host "Package: $OutputDirectory"
Write-Host "Architecture: $Architecture"
Write-Host "App version: $($versionInfo.Version)"
Write-Host "Minimum Windows: $minimumWindowsLabel"
Write-Host "Config source: $ConfigPath"
Write-Host "Size: $([Math]::Round($packageSize / 1MB, 1)) MB"
Write-Host 'Copy the entire package directory to the target computer.'

$cloudZipName = Get-SearchEngineCloudZipName `
    -PackageLeaf $packageName `
    -ReleaseId $CloudReleaseId
$cloudHelper = & (Join-Path $PSScriptRoot 'Find-WorkspaceReleaseRoot.ps1') `
    -Name 'Publish-ReleasePackageIfConfigured.ps1' -StartPath $projectRoot `
    -Optional
if ($null -ne $cloudHelper) {
    $cloudArgs = @{
        PackageDirectory = $OutputDirectory
        ProductName = 'ZagEditor'
        ZipName = $cloudZipName
        ReleaseId = $CloudReleaseId
        StartPath = $projectRoot
        SkipCloudPublish = $SkipCloudPublish
    }
    if (-not [string]::IsNullOrWhiteSpace($CloudRoot)) {
        $cloudArgs.CloudRoot = $CloudRoot
    }
    & $cloudHelper @cloudArgs | Out-Null
}
else {
    Write-Host 'Cloud publish helper not found; local package only.'
}
