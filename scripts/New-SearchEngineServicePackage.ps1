[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'x86-modern')]
    [string]$Architecture = 'x64',
    [string]$BuildDirectory,
    [string]$SettingsPath,
    [string]$IgnorePath,
    [string]$OutputDirectory,
    [string]$VCRedistPath,
    [string]$CloudRoot,
    [string]$CloudReleaseId,
    [switch]$SkipCloudPublish
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')
. (Join-Path $PSScriptRoot 'ConsoleScriptEncoding.ps1')
$templateRoot = Join-Path $projectRoot 'deployment\SearchEngineServicePortable'
$sourceDataRoot = Join-Path $templateRoot 'source-data'
$packageMarkerName = '.searchengine-portable-package'
$expectedOemHash = 'EA765FC534362C43EAFA65F4B004F60FF5DE11865E39CC67DCC2042A7F71B0D2'
$productName = 'SearchEngineService'
$versionInfo = Get-SearchEngineAppVersionNames `
    -ProjectRoot $projectRoot `
    -ProductName $productName
if ([string]::IsNullOrWhiteSpace($CloudReleaseId)) {
    $CloudReleaseId = $versionInfo.ReleaseId
}

if ($Architecture -eq 'x86') {
    # Legacy Win7 SP1 portable (VS2019/v142). Keep -Architecture x86 meaning.
    $packageName = 'SearchEngineService-x86-Windows7'
    $minimumWindowsVersion = '6.1'
    $minimumWindowsLabel = 'Windows 7 SP1'
    $expectedMachine = 0x014c
    $buildPresetDirectory = 'windows7-x86'
    $visualStudioVersionRange = '[16.0,17.0)'
    $expectedToolset = 'v142'
} elseif ($Architecture -eq 'x86-modern') {
    # Modern Win32 on VS2022/v143 (CMake preset windows-x86).
    $packageName = 'SearchEngineService-x86'
    $minimumWindowsVersion = '10.0'
    $minimumWindowsLabel = 'Windows 10 / Windows Server 2016'
    $expectedMachine = 0x014c
    $buildPresetDirectory = 'windows-x86'
    $visualStudioVersionRange = '[17.0,18.0)'
    $expectedToolset = 'v143'
} else {
    $packageName = 'SearchEngineService-x64'
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

function Get-Sha256FromBytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
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
if ([string]::IsNullOrWhiteSpace($SettingsPath)) {
    $SettingsPath = Join-Path $sourceDataRoot 'Settings.json'
}
if ([string]::IsNullOrWhiteSpace($IgnorePath)) {
    $IgnorePath = Join-Path $sourceDataRoot 'ignore.txt'
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
    (Join-Path $BuildDirectory 'SearchEngine.exe') 'SearchEngine Release binary'
$configToolPath = Resolve-RequiredFile `
    (Join-Path $BuildDirectory 'SearchEngineConfig.exe') `
    'SearchEngineConfig Release helper'
Assert-PeMatchesAppVersion `
    -BinaryPath $binaryPath `
    -ExpectedProductVersion $versionInfo.Version `
    -ExpectedFileVersion $versionInfo.FileVersion `
    -ExpectedProductName $productName `
    -ExpectedOriginalFilename 'SearchEngine.exe'
Assert-PeMatchesAppVersion `
    -BinaryPath $configToolPath `
    -ExpectedProductVersion $versionInfo.Version `
    -ExpectedFileVersion $versionInfo.FileVersion `
    -ExpectedProductName $productName `
    -ExpectedOriginalFilename 'SearchEngineConfig.exe'
$SettingsPath = Resolve-RequiredFile $SettingsPath 'Portable Settings.json'
$IgnorePath = Resolve-RequiredFile $IgnorePath 'Portable ignore.txt'
$VCRedistPath = Resolve-RequiredFile $VCRedistPath `
    "Microsoft Visual C++ Redistributable $vcRedistArch"
$oemBase64Path = Resolve-RequiredFile `
    (Join-Path $sourceDataRoot 'OEM866.INI.base64') 'OEM866 table source'

$machine = Get-PeMachine $binaryPath
if ($machine -ne $expectedMachine) {
    throw ('SearchEngine.exe architecture mismatch: expected 0x{0:X4}, got 0x{1:X4}.' `
        -f $expectedMachine, $machine)
}
$configToolMachine = Get-PeMachine $configToolPath
if ($configToolMachine -ne $expectedMachine) {
    throw ('SearchEngineConfig.exe architecture mismatch: expected 0x{0:X4}, got 0x{1:X4}.' `
        -f $expectedMachine, $configToolMachine)
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

$signature = Get-AuthenticodeSignature -LiteralPath $VCRedistPath
if ($signature.Status -ne 'Valid' -or
    $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "VC++ Redistributable does not have a valid Microsoft signature: $VCRedistPath"
}
if ($Architecture -eq 'x86' -and
    (Get-Item -LiteralPath $VCRedistPath).VersionInfo.FileVersion -notlike '14.29.*') {
    throw 'The Windows 7 package requires the VS2019 VC++ Runtime 14.29 x86.'
}

$settings = Get-Content -LiteralPath $SettingsPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$config = $settings.config
if (-not $config -or -not $config.Name -or -not $config.year -or
    -not $config.dirs -or $config.dirs.Count -eq 0 -or
    -not $config.extensions -or $config.extensions.Count -eq 0) {
    throw "Settings.json is missing required config fields: $SettingsPath"
}
$port = if ($config.port) { [int]$config.port } else { [int]$config.asio_port }
if ($port -lt 1 -or $port -gt 65535) {
    throw "Settings.json contains an invalid ASIO port: $port"
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
    foreach ($directory in @('app', 'data', 'tools', 'prerequisites')) {
        New-Item -ItemType Directory `
            -Path (Join-Path $stagingDirectory $directory) | Out-Null
    }

    Copy-Item -LiteralPath $binaryPath `
        -Destination (Join-Path $stagingDirectory 'app\SearchEngine.exe')
    Copy-Item -LiteralPath $configToolPath `
        -Destination (Join-Path $stagingDirectory 'tools\SearchEngineConfig.exe')
    Copy-Item -LiteralPath $SettingsPath `
        -Destination (Join-Path $stagingDirectory 'data\Settings.json')
    Copy-Item -LiteralPath $IgnorePath `
        -Destination (Join-Path $stagingDirectory 'data\ignore.txt')

    $oemBase64 = (Get-Content -LiteralPath $oemBase64Path -Raw).Trim()
    $oemBytes = [Convert]::FromBase64String($oemBase64)
    $actualOemHash = Get-Sha256FromBytes $oemBytes
    if ($actualOemHash -ne $expectedOemHash) {
        throw 'The embedded OEM866 table failed its SHA-256 check.'
    }
    [IO.File]::WriteAllBytes(
        (Join-Path $stagingDirectory 'data\OEM866.INI'),
        $oemBytes
    )

    $portableBatchFiles = @(
        @{
            Source = 'Install-SearchEngineService-Windows7.bat'
            Destination = 'Install-SearchEngineService.bat'
        },
        @{
            Source = 'Stop-SearchEngineService-Windows7.bat'
            Destination = 'Stop-SearchEngineService.bat'
        },
        @{
            Source = 'Start-SearchEngineService-Windows7.bat'
            Destination = 'Start-SearchEngineService.bat'
        },
        @{
            Source = 'Restart-SearchEngineService-Windows7.bat'
            Destination = 'Restart-SearchEngineService.bat'
        },
        @{
            Source = 'Uninstall-SearchEngineService-Windows7.bat'
            Destination = 'Uninstall-SearchEngineService.bat'
        },
        @{
            Source = 'Verify-Package-Windows7.bat'
            Destination = 'Verify-Package.bat'
        }
    )
    foreach ($batchFile in $portableBatchFiles) {
        Copy-Item `
            -LiteralPath (Join-Path $templateRoot $batchFile.Source) `
            -Destination (Join-Path $stagingDirectory $batchFile.Destination)
    }
    Copy-Item -LiteralPath (Join-Path $templateRoot 'ServiceInstance.cmd') `
        -Destination (Join-Path $stagingDirectory 'ServiceInstance.cmd')
    Get-ChildItem -LiteralPath $stagingDirectory -Recurse -File |
        Where-Object { $_.Extension -in @('.bat', '.cmd') } |
        ForEach-Object {
            $batchText = [IO.File]::ReadAllText($_.FullName)
            $batchText = $batchText.Replace('{{ARCHITECTURE}}', $Architecture)
            $batchText = $batchText.Replace('{{VC_REDIST_FILE}}', $vcRedistName)
            $batchText = ($batchText -replace "`r?`n", "`r`n")
            # Portable .bat/.cmd must stay strict ASCII for cmd.exe / Windows 7.
            # Fail loudly instead of silently replacing non-ASCII with '?'.
            Write-StrictAsciiText -Path $_.FullName -Text $batchText
        }

    $textTokens = @{
        '{{ARCHITECTURE}}' = $Architecture
        '{{MINIMUM_WINDOWS}}' = $minimumWindowsLabel
        '{{PACKAGE_NAME}}' = $packageName
        '{{VC_REDIST_FILE}}' = $vcRedistName
    }
    $textTemplates = @{
        'README.txt' = 'README.txt'
        'INSTALLATION_GUIDE_RU.txt' = 'INSTALLATION_GUIDE_RU.txt'
    }
    foreach ($name in $textTemplates.Keys) {
        $text = Get-Content -LiteralPath `
            (Join-Path $templateRoot $textTemplates[$name]) `
            -Raw -Encoding UTF8
        foreach ($token in $textTokens.Keys) {
            $text = $text.Replace($token, $textTokens[$token])
        }
        [IO.File]::WriteAllText(
            (Join-Path $stagingDirectory $name),
            $text,
            (New-Object Text.UTF8Encoding($true))
        )
    }

    Copy-Item -LiteralPath $VCRedistPath -Destination `
        (Join-Path $stagingDirectory ('prerequisites\' + $vcRedistName))

    New-Item -ItemType Directory `
        -Path (Join-Path $stagingDirectory 'data\logs') | Out-Null
    New-Item -ItemType Directory `
        -Path (Join-Path $stagingDirectory 'data\messages') | Out-Null

    $protectedFiles = @()
    foreach ($relativeRoot in @('app', 'tools', 'prerequisites')) {
        $absoluteRoot = Join-Path $stagingDirectory $relativeRoot
        $protectedFiles += Get-ChildItem -LiteralPath $absoluteRoot `
            -Recurse -File
    }
    $protectedFiles += Get-Item -LiteralPath `
        (Join-Path $stagingDirectory 'data\OEM866.INI')
    foreach ($name in @(
        'Install-SearchEngineService.bat',
        'Stop-SearchEngineService.bat',
        'Start-SearchEngineService.bat',
        'Restart-SearchEngineService.bat',
        'Uninstall-SearchEngineService.bat',
        'Verify-Package.bat'
    )) {
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
    if ($manifestEntries.path -contains 'data/Settings.json') {
        throw 'Mutable data/Settings.json must not be checksum-protected.'
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
        product = 'SearchEngineService'
        architecture = $Architecture
        applicationVersion = $versionInfo.Version
        fileVersion = $versionInfo.FileVersion
        releaseId = $CloudReleaseId
        minimumWindowsVersion = $minimumWindowsVersion
        toolset = $expectedToolset
        vcRuntimeFile = $vcRedistName
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
        "Generated by New-SearchEngineServicePackage.ps1`r`n",
        (New-Object Text.UTF8Encoding($false))
    )

    if (Test-Path -LiteralPath $OutputDirectory) {
        $existingMarker = Join-Path $OutputDirectory $packageMarkerName
        if (-not (Test-Path -LiteralPath $existingMarker -PathType Leaf)) {
            throw "Existing output has no package marker and was preserved: $OutputDirectory"
        }

        # Rename the old package first. If another program keeps the directory
        # open, Rename-Item fails before any package files can be removed.
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
Write-Host "Settings source: $SettingsPath"
Write-Host "Size: $([Math]::Round($packageSize / 1MB, 1)) MB"
Write-Host 'Copy the entire package directory to the target computer.'

$cloudZipName = Get-SearchEngineCloudZipName `
    -PackageLeaf $packageName `
    -ReleaseId $CloudReleaseId
$cloudHelper = @(
    (Join-Path $projectRoot '..\..\TOOLS\scripts\release\Publish-ReleasePackageIfConfigured.ps1'),
    (Join-Path $projectRoot '..\..\..\TOOLS\scripts\release\Publish-ReleasePackageIfConfigured.ps1')
) | ForEach-Object { [IO.Path]::GetFullPath($_) } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if ($null -ne $cloudHelper) {
    $cloudArgs = @{
        PackageDirectory = $OutputDirectory
        ProductName = 'SearchEngineService'
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
