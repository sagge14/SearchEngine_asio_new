[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'x86', 'x86-modern')]
    [string]$Architecture,

    [Parameter(Mandatory)]
    [string]$BuildDirectory,

    [Parameter(Mandatory)]
    [string]$Configuration,

    [switch]$SkipCloudPublish
)

$ErrorActionPreference = 'Stop'

if ($Configuration -ne 'Release') {
    Write-Host (
        "ZagEditor package PostBuild skipped " +
        "(Configuration=$Configuration; only Release is packaged)."
    )
    return
}

& (Join-Path $PSScriptRoot 'New-ZagEditorPackage.ps1') `
    -Architecture $Architecture `
    -BuildDirectory $BuildDirectory `
    -SkipCloudPublish:$SkipCloudPublish
