[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'all')]
    [string] $Architecture = 'all',

    [string] $BoostSource = 'C:\Boost\boost_1_85_0\boost_1_85_0',

    [string] $InstallRoot = 'C:\Boost',

    [ValidateSet('msvc-14.3', 'msvc-14.2')]
    [string] $Toolset = 'msvc-14.3',

    [string] $BuildRoot = (
        Join-Path ([System.IO.Path]::GetTempPath()) 'SearchEngine-boost-build'
    ),

    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$boostSourcePath = [System.IO.Path]::GetFullPath($BoostSource)
$installRootPath = [System.IO.Path]::GetFullPath($InstallRoot)
$buildRootPath = [System.IO.Path]::GetFullPath($BuildRoot)
$b2Path = Join-Path $boostSourcePath 'b2.exe'

if (-not (Test-Path -LiteralPath $boostSourcePath -PathType Container)) {
    throw "Boost source directory does not exist: $boostSourcePath"
}

if (-not (Test-Path -LiteralPath $b2Path -PathType Leaf)) {
    $bootstrapPath = Join-Path $boostSourcePath 'bootstrap.bat'
    if (-not (Test-Path -LiteralPath $bootstrapPath -PathType Leaf)) {
        throw "Neither b2.exe nor bootstrap.bat exists in $boostSourcePath"
    }

    $bootstrapToolset = switch ($Toolset) {
        'msvc-14.3' { 'vc143' }
        'msvc-14.2' { 'vc142' }
    }

    Write-Host "Bootstrapping Boost.Build with $bootstrapToolset"
    Push-Location $boostSourcePath
    try {
        & $bootstrapPath $bootstrapToolset
        if ($LASTEXITCODE -ne 0) {
            throw "Boost bootstrap failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

$architectures = switch ($Architecture) {
    'all' { @('x64', 'x86') }
    default { @($Architecture) }
}

foreach ($targetArchitecture in $architectures) {
    if ($targetArchitecture -eq 'x64') {
        $addressModel = '64'
        $installDirectory = Join-Path $installRootPath 'windows'
    }
    else {
        $addressModel = '32'
        $installDirectory = Join-Path $installRootPath 'windows32'
    }

    $buildDirectory = Join-Path $buildRootPath $targetArchitecture
    New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
    New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null

    $b2Arguments = @(
        "--build-dir=$buildDirectory"
        "--prefix=$installDirectory"
        "--layout=versioned"
        "--with-system"
        "--with-filesystem"
        "--with-serialization"
        "toolset=$Toolset"
        'architecture=x86'
        "address-model=$addressModel"
        'variant=debug,release'
        'link=static'
        'runtime-link=shared'
        'threading=multi'
        'install'
        "-j$Jobs"
    )

    Write-Host ""
    Write-Host "Building Boost $targetArchitecture into $installDirectory"
    Write-Host "Debug: /MDd, runtime-debugging=on, iterator debug level 2"
    Write-Host "Release: /MD, runtime-debugging=off, iterator debug level 0"

    & $b2Path @b2Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Boost $targetArchitecture build failed with exit code $LASTEXITCODE"
    }
}

Write-Host ""
Write-Host 'Boost build completed.'
Write-Host 'Reconfigure SearchEngine from a clean preset build directory.'
