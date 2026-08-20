#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$TemplatePath,

    [Parameter(Mandatory)]
    [string]$OutputPath,

    [Nullable[int]]$Year,

    [string]$AllowedOutputRoot,

    [string]$ConfigToolPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-FullPath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'Path must not be empty.'
    }
    if (-not [IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path $BasePath $Path
    }
    return [IO.Path]::GetFullPath($Path)
}

function Test-JsonInteger($Value) {
    return (
        $Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64]
    )
}

$basePath = (Get-Location).Path
$TemplatePath = Resolve-FullPath $TemplatePath $basePath
$OutputPath = Resolve-FullPath $OutputPath $basePath

if (-not (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) {
    throw "Release Settings template was not found: $TemplatePath"
}
if ($TemplatePath.Equals($OutputPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputPath must differ from TemplatePath; the tracked template is read-only.'
}

$outputDirectory = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    throw "Release Settings output directory was not found: $outputDirectory"
}

if (-not [string]::IsNullOrWhiteSpace($AllowedOutputRoot)) {
    $allowedRoot = Resolve-FullPath $AllowedOutputRoot $basePath
    if (-not (Test-Path -LiteralPath $allowedRoot -PathType Container)) {
        throw "AllowedOutputRoot was not found: $allowedRoot"
    }
    $allowedRoot = $allowedRoot.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $allowedPrefix = $allowedRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $OutputPath.StartsWith(
        $allowedPrefix,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw "OutputPath must stay inside AllowedOutputRoot: $allowedRoot"
    }
}

$selectedYear = if ($PSBoundParameters.ContainsKey('Year')) {
    [int]$Year
} else {
    (Get-Date).Year
}
if ($selectedYear -lt 2000 -or $selectedYear -gt 2099) {
    throw "Year must be inside 2000..2099; got $selectedYear."
}

try {
    $settings = Get-Content -LiteralPath $TemplatePath -Raw -Encoding UTF8 |
        ConvertFrom-Json
} catch {
    throw "Release Settings template is not valid JSON: $TemplatePath. $($_.Exception.Message)"
}

$configProperty = $settings.PSObject.Properties['config']
if ($null -eq $configProperty -or
    $configProperty.Value -isnot [PSCustomObject]) {
    throw 'Release Settings template must contain the canonical config object.'
}
$config = $configProperty.Value

$yearProperty = $config.PSObject.Properties['year']
if ($null -eq $yearProperty) {
    throw 'Release Settings template is missing canonical config.year.'
}
if ($yearProperty.Value -isnot [string] -or
    $yearProperty.Value -notmatch '^\d{4}$') {
    throw 'Release Settings template config.year must be a four-digit string.'
}
$templateYear = [int]$yearProperty.Value
if ($templateYear -lt 2000 -or $templateYear -gt 2099) {
    throw 'Release Settings template config.year must be inside 2000..2099.'
}

$portProperty = $config.PSObject.Properties['asio_port']
if ($null -eq $portProperty) {
    throw 'Release Settings template is missing canonical config.asio_port.'
}
if (-not (Test-JsonInteger $portProperty.Value)) {
    throw 'Release Settings template config.asio_port must be an integer.'
}
$basePort = [long]$portProperty.Value
if ($basePort -lt 1 -or $basePort -gt 65535) {
    throw "Release Settings template config.asio_port must be inside 1..65535; got $basePort."
}

$generatedPort = [long]([Math]::Floor($basePort / 10)) * 10 +
    ($selectedYear % 10)
if ($generatedPort -lt 1 -or $generatedPort -gt 65535) {
    throw "Generated release port must be inside 1..65535; got $generatedPort."
}

$config.year = $selectedYear.ToString(
    [Globalization.CultureInfo]::InvariantCulture
)
$config.asio_port = [int]$generatedPort

$jsonText = $settings | ConvertTo-Json -Depth 100
$jsonText = ($jsonText -replace "`r?`n", "`r`n").TrimEnd("`r", "`n") + "`r`n"
$utf8NoBom = New-Object Text.UTF8Encoding($false)
$temporaryPath = Join-Path $outputDirectory (
    '.' + [IO.Path]::GetFileName($OutputPath) + '.tmp-' +
    [Guid]::NewGuid().ToString('N')
)

try {
    [IO.File]::WriteAllText($temporaryPath, $jsonText, $utf8NoBom)

    if (-not [string]::IsNullOrWhiteSpace($ConfigToolPath)) {
        $ConfigToolPath = Resolve-FullPath $ConfigToolPath $basePath
        if (-not (Test-Path -LiteralPath $ConfigToolPath -PathType Leaf)) {
            throw "SearchEngineConfig validator was not found: $ConfigToolPath"
        }
        $validationOutput = & $ConfigToolPath validate --settings $temporaryPath 2>&1
        $validationExitCode = $LASTEXITCODE
        if ($validationExitCode -ne 0) {
            $validationSummary = ($validationOutput | ForEach-Object {
                $_.ToString().Trim()
            } | Where-Object { $_ }) -join ' '
            throw (
                "Canonical Settings validation failed with exit code " +
                "$validationExitCode. $validationSummary"
            )
        }
    }

    if (Test-Path -LiteralPath $OutputPath -PathType Leaf) {
        [IO.File]::Replace($temporaryPath, $OutputPath, $null, $true)
    } else {
        [IO.File]::Move($temporaryPath, $OutputPath)
    }
} finally {
    if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}

Write-Host (
    "Release Settings prepared: year=$selectedYear; " +
    "base_port=$basePort; generated_port=$generatedPort; output=$OutputPath"
)

[pscustomobject]@{
    Year = $selectedYear
    BasePort = [int]$basePort
    GeneratedPort = [int]$generatedPort
    OutputPath = $OutputPath
}
