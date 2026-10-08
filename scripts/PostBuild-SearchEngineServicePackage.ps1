[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'x86', 'x86-modern')]
    [string]$Architecture,

    [Parameter(Mandatory)]
    [string]$BuildDirectory,

    [Parameter(Mandatory)]
    [string]$Configuration,

    [switch]$SkipCloudPublish,

    [Nullable[int]]$Year
)

$ErrorActionPreference = 'Stop'

if ($Configuration -ne 'Release') {
    Write-Host (
        "SearchEngineService package PostBuild skipped " +
        "(Configuration=$Configuration; only Release is packaged)."
    )
    return
}

$packageArguments = @{
    Architecture = $Architecture
    BuildDirectory = $BuildDirectory
    SkipCloudPublish = $SkipCloudPublish
}
if ($PSBoundParameters.ContainsKey('Year')) {
    $packageArguments.Year = [int]$Year
}

& (Join-Path $PSScriptRoot 'New-SearchEngineServicePackage.ps1') `
    @packageArguments
