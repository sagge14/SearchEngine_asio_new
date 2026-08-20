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
    [switch]$SkipCloudPublish,
    [Nullable[int]]$Year
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'AppVersion.ps1')
. (Join-Path $PSScriptRoot 'ConsoleScriptEncoding.ps1')
. (Join-Path $PSScriptRoot 'Assert-SearchEngineConfigAutoPadContract.ps1')
$templateRoot = Join-Path $projectRoot 'deployment\SearchEngineServicePortable'
$sourceDataRoot = Join-Path $templateRoot 'source-data'
$packageMarkerName = '.searchengine-portable-package'
# Two CP866 lines of 33 letters (lowercase then uppercase, CRLF), including ц/Ц.
$expectedOemHash = '42E5905CB86ACE474F11B11F5A9DF49E4A25F6FF8E0D2FFB7A7BFC8A7ED29260'
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

function Assert-Oem866Table([byte[]]$Bytes) {
    $splitAt = -1
    for ($i = 0; $i -lt ($Bytes.Length - 1); $i++) {
        if ($Bytes[$i] -eq 0x0D -and $Bytes[$i + 1] -eq 0x0A) {
            $splitAt = $i
            break
        }
    }
    if ($splitAt -lt 0) {
        throw 'OEM866.INI must contain two CRLF-separated CP866 alphabet lines.'
    }
    $lowerLength = $splitAt
    $upperLength = $Bytes.Length - $splitAt - 2
    if ($lowerLength -ne 33 -or $upperLength -ne 33) {
        throw ("OEM866.INI must contain two 33-byte CP866 alphabet lines; got {0} and {1}." `
            -f $lowerLength, $upperLength)
    }
    # Alphabet order places ц/Ц immediately after х/Х (index 23).
    if ($Bytes[23] -ne 0xE6 -or $Bytes[$splitAt + 2 + 23] -ne 0x96) {
        throw 'OEM866.INI is missing CP866 letters ц/Ц after х/Х.'
    }
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
Assert-SearchEngineConfigSourceFreshness `
    -ConfigToolPath $configToolPath `
    -ProjectRoot $projectRoot
$authDbToolPath = Resolve-RequiredFile `
    (Join-Path $BuildDirectory 'AuthDbTool.exe') `
    'AuthDbTool Release helper'
$tokenIssuerPath = Resolve-RequiredFile `
    (Join-Path $BuildDirectory 'SearchClientTokenIssuer.exe') `
    'SearchClientTokenIssuer Release helper'
$tokenIssuerDefaultsPath = Resolve-RequiredFile `
    (Join-Path $BuildDirectory 'searchclient-auth-token.defaults.json') `
    'SearchClientTokenIssuer defaults JSON'
$registerAuthScriptPath = Resolve-RequiredFile `
    (Join-Path $projectRoot 'scripts\Register-AuthClientFromToken.ps1') `
    'Register-AuthClientFromToken.ps1'
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
$settingsTemplatePath = Resolve-RequiredFile `
    $SettingsPath 'Portable Settings.json template'
$IgnorePath = Resolve-RequiredFile $IgnorePath 'Portable ignore.txt'
$VCRedistPath = Resolve-RequiredFile $VCRedistPath `
    "Microsoft Visual C++ Redistributable $vcRedistArch"
$oemBase64Path = Resolve-RequiredFile `
    (Join-Path $sourceDataRoot 'OEM866.INI.base64') 'OEM866 table source'
$PrefixMapPath = Resolve-RequiredFile `
    (Join-Path $sourceDataRoot 'prefix_map.json') 'Portable prefix_map.json'

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
$authDbToolMachine = Get-PeMachine $authDbToolPath
if ($authDbToolMachine -ne $expectedMachine) {
    throw ('AuthDbTool.exe architecture mismatch: expected 0x{0:X4}, got 0x{1:X4}.' `
        -f $expectedMachine, $authDbToolMachine)
}
$tokenIssuerMachine = Get-PeMachine $tokenIssuerPath
if ($tokenIssuerMachine -ne $expectedMachine) {
    throw ('SearchClientTokenIssuer.exe architecture mismatch: expected 0x{0:X4}, got 0x{1:X4}.' `
        -f $expectedMachine, $tokenIssuerMachine)
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

    $generatedSettingsPath = Join-Path $stagingDirectory 'data\Settings.json'
    $prepareArguments = @{
        TemplatePath = $settingsTemplatePath
        OutputPath = $generatedSettingsPath
        AllowedOutputRoot = $stagingDirectory
        ConfigToolPath = $configToolPath
    }
    if ($PSBoundParameters.ContainsKey('Year')) {
        $prepareArguments.Year = [int]$Year
    }
    $preparedSettings = & (Join-Path $PSScriptRoot `
        'Prepare-YearBasedReleaseSettings.ps1') @prepareArguments
    $releaseSettingsYear = [int]$preparedSettings.Year

    & $configToolPath validate --settings $generatedSettingsPath
    if ($LASTEXITCODE -ne 0) {
        throw "SearchEngineConfig rejected generated portable Settings.json: $generatedSettingsPath"
    }
    & $configToolPath validate-prefix-map --path $PrefixMapPath
    if ($LASTEXITCODE -ne 0) {
        throw "SearchEngineConfig rejected portable prefix_map.json: $PrefixMapPath"
    }
    Assert-SearchEngineConfigAutoPadContract `
        -ConfigToolPath $configToolPath `
        -TemplatePath $generatedSettingsPath

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

    $settings = Get-Content -LiteralPath $generatedSettingsPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $config = $settings.config
    $indexRootsProperty = if ($null -ne $config) {
        $config.PSObject.Properties['index_roots']
    } else {
        $null
    }
    $dirsProperty = if ($null -ne $config) {
        $config.PSObject.Properties['dirs']
    } else {
        $null
    }
    $indexRoots = if ($null -ne $indexRootsProperty) {
        @($indexRootsProperty.Value)
    } elseif ($null -ne $dirsProperty) {
        @($dirsProperty.Value)
    } else {
        @()
    }
    $indexedExtensionsProperty = if ($null -ne $config) {
        $config.PSObject.Properties['indexed_extensions']
    } else {
        $null
    }
    $legacyExtensionsProperty = if ($null -ne $config) {
        $config.PSObject.Properties['extensions']
    } else {
        $null
    }
    $includeExtensionlessProperty = if ($null -ne $config) {
        $config.PSObject.Properties['include_extensionless_files']
    } else {
        $null
    }
    $hasIndexedFileTypes = if ($null -ne $indexedExtensionsProperty) {
        $indexedExtensionsValue = $indexedExtensionsProperty.Value
        $includeExtensionlessIsValid = (
            $null -eq $includeExtensionlessProperty -or
            $includeExtensionlessProperty.Value -is [bool]
        )
        $includeExtensionless = (
            $includeExtensionlessIsValid -and
            $null -ne $includeExtensionlessProperty -and
            $includeExtensionlessProperty.Value
        )
        $indexedExtensionsValue -is [System.Array] -and
            $includeExtensionlessIsValid -and
            ($indexedExtensionsValue.Count -gt 0 -or $includeExtensionless)
    } elseif ($null -ne $legacyExtensionsProperty) {
        $legacyExtensionsProperty.Value -is [System.Array] -and
            $legacyExtensionsProperty.Value.Count -gt 0
    } else {
        $false
    }
    if (-not $config -or -not $config.year -or
        $indexRoots.Count -eq 0 -or
        -not $hasIndexedFileTypes) {
        throw "Generated Settings.json is missing required config fields: $generatedSettingsPath"
    }
    $port = [int]$config.asio_port
    if ($port -lt 1 -or $port -gt 65535) {
        throw "Settings.json contains an invalid ASIO port: $port"
    }

    Copy-Item -LiteralPath $binaryPath `
        -Destination (Join-Path $stagingDirectory 'app\SearchEngine.exe')
    Copy-Item -LiteralPath $configToolPath `
        -Destination (Join-Path $stagingDirectory 'tools\SearchEngineConfig.exe')
    Copy-Item -LiteralPath $authDbToolPath `
        -Destination (Join-Path $stagingDirectory 'tools\AuthDbTool.exe')
    Copy-Item -LiteralPath $tokenIssuerPath `
        -Destination (Join-Path $stagingDirectory 'tools\SearchClientTokenIssuer.exe')
    Copy-Item -LiteralPath $tokenIssuerDefaultsPath `
        -Destination (Join-Path $stagingDirectory `
            'tools\searchclient-auth-token.defaults.json')
    # OpenSSL runtime DLL: SearchEngine.exe (auth verify) and TokenIssuer both
    # import libcrypto dynamically. Ship it next to each EXE — the service runs
    # from install\bin, tools stay in install\tools, and Windows does not search
    # across those folders.
    $opensslDllName = if ($Architecture -eq 'x64') {
        'libcrypto-3-x64.dll'
    } else {
        'libcrypto-3.dll'
    }
    $opensslDllCandidates = @(
        (Join-Path $BuildDirectory $opensslDllName),
        (Join-Path ${env:ProgramFiles} "OpenSSL-Win64\bin\$opensslDllName"),
        (Join-Path ${env:ProgramFiles} "OpenSSL\bin\$opensslDllName"),
        (Join-Path ${env:ProgramFiles(x86)} "OpenSSL-Win32\bin\$opensslDllName"),
        (Join-Path ${env:ProgramFiles(x86)} "OpenSSL-Win32\bin\libcrypto-3.dll")
    )
    $opensslDll = $opensslDllCandidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
    if (-not $opensslDll) {
        throw "OpenSSL DLL '$opensslDllName' was not found for SearchEngine/TokenIssuer packaging."
    }
    $opensslFileName = [IO.Path]::GetFileName($opensslDll)
    Copy-Item -LiteralPath $opensslDll `
        -Destination (Join-Path $stagingDirectory ("app\" + $opensslFileName))
    Copy-Item -LiteralPath $opensslDll `
        -Destination (Join-Path $stagingDirectory ("tools\" + $opensslFileName))
    Copy-Item -LiteralPath $registerAuthScriptPath `
        -Destination (Join-Path $stagingDirectory 'tools\Register-AuthClientFromToken.ps1')
    Copy-Item -LiteralPath $IgnorePath `
        -Destination (Join-Path $stagingDirectory 'data\ignore.txt')
    Copy-Item -LiteralPath $PrefixMapPath `
        -Destination (Join-Path $stagingDirectory 'data\prefix_map.json')

    $oemBase64 = (Get-Content -LiteralPath $oemBase64Path -Raw).Trim()
    $oemBytes = [Convert]::FromBase64String($oemBase64)
    Assert-Oem866Table $oemBytes
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
            Source = 'Register-AuthClient-FromToken-Windows7.bat'
            Destination = 'Register-AuthClient-FromToken.bat'
        },
        @{
            Source = 'Issue-SearchClientToken-Windows7.bat'
            Destination = 'Issue-SearchClientToken.bat'
        },
        @{
            Source = 'Verify-Package-Windows7.bat'
            Destination = 'Verify-Package.bat'
        },
        @{
            Source = 'Configure-SearchEngineService.bat'
            Destination = 'Configure-SearchEngineService.bat'
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

    $protectedFiles = @()
    foreach ($relativeRoot in @('app', 'tools', 'prerequisites')) {
        $absoluteRoot = Join-Path $stagingDirectory $relativeRoot
        $protectedFiles += Get-ChildItem -LiteralPath $absoluteRoot `
            -Recurse -File
    }
    foreach ($name in @(
        'Install-SearchEngineService.bat',
        'Stop-SearchEngineService.bat',
        'Start-SearchEngineService.bat',
        'Restart-SearchEngineService.bat',
        'Uninstall-SearchEngineService.bat',
        'Register-AuthClient-FromToken.bat',
        'Issue-SearchClientToken.bat',
        'Verify-Package.bat',
        'Configure-SearchEngineService.bat'
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
    $protectedData = @($manifestEntries.path | Where-Object { $_ -like 'data/*' })
    if ($protectedData.Count -gt 0) {
        throw ('Mutable data files must not be checksum-protected: ' +
            ($protectedData -join ', '))
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
Write-Host "Settings template: $settingsTemplatePath"
Write-Host "Generated Settings year: $releaseSettingsYear"
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
