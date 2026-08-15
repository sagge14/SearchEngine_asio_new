#Requires -Version 5.1
<#
.SYNOPSIS
Locates the workspace TOOLS release helper directory from a project checkout.

.DESCRIPTION
This is the one release bootstrap that cannot live in TOOLS, because it is what
finds TOOLS. Resolution order:

  1. WORKSPACE_TOOLS_ROOT environment variable (process scope, then User scope).
  2. Every parent directory of the project, looking for TOOLS\scripts\release.

The parent walk has no fixed depth, so the same checkout works regardless of
where the workspace lives on a given machine and how deeply the project is
nested inside it.

Deploy this file to <Project>\scripts\Find-WorkspaceReleaseRoot.ps1. Callers
locate it through $PSScriptRoot, which is always known.

.PARAMETER Name
Release helper file name to resolve inside TOOLS\scripts\release. When omitted,
the release directory itself is returned.

.PARAMETER StartPath
Directory to start the upward walk from. Defaults to the project root, i.e. the
parent of the directory holding this script.

.PARAMETER ToolsRoot
Return the TOOLS root itself instead of its scripts\release directory. Use this
for non-release workspace assets such as dependencies\sqlite.

.PARAMETER Optional
Return $null instead of throwing when TOOLS or the named helper is missing.

.EXAMPLE
$resolveScript = & (Join-Path $PSScriptRoot 'Find-WorkspaceReleaseRoot.ps1') `
    -Name 'Resolve-WorkspaceReleaseScript.ps1' -StartPath $project
#>
[CmdletBinding()]
param(
    [string]$Name,
    [string]$StartPath,
    [switch]$ToolsRoot,
    [switch]$Optional
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not [string]::IsNullOrWhiteSpace($Name) -and
    $Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.ps1$') {
    throw "Unsafe release helper name: $Name"
}

if ([string]::IsNullOrWhiteSpace($StartPath)) {
    $StartPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
}
$StartPath = [IO.Path]::GetFullPath($StartPath)

function Test-WorkspaceReleaseRoot {
    param([string]$Candidate)

    if ([string]::IsNullOrWhiteSpace($Candidate)) { return $false }
    return Test-Path -PathType Leaf -LiteralPath (
        Join-Path $Candidate 'Resolve-WorkspaceReleaseScript.ps1')
}

$releaseRoot = $null

# An explicit root wins over layout. It may point either at TOOLS itself or
# directly at TOOLS\scripts\release.
foreach ($configured in @(
    $env:WORKSPACE_TOOLS_ROOT,
    [Environment]::GetEnvironmentVariable('WORKSPACE_TOOLS_ROOT', 'User')
)) {
    if ([string]::IsNullOrWhiteSpace($configured)) { continue }
    foreach ($suffix in @('scripts\release', '')) {
        $candidate = [IO.Path]::GetFullPath((Join-Path $configured $suffix))
        if (Test-WorkspaceReleaseRoot $candidate) {
            $releaseRoot = $candidate
            break
        }
    }
    if ($null -ne $releaseRoot) { break }
}

if ($null -eq $releaseRoot) {
    $current = $StartPath
    while ($true) {
        $candidate = [IO.Path]::GetFullPath(
            (Join-Path $current 'TOOLS\scripts\release'))
        if (Test-WorkspaceReleaseRoot $candidate) {
            $releaseRoot = $candidate
            break
        }
        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current) {
            break
        }
        $current = $parent
    }
}

if ($null -eq $releaseRoot) {
    if ($Optional) { return $null }
    throw (
        "Workspace TOOLS not found in any parent of '$StartPath'. " +
        'Clone the TOOLS repository so that TOOLS\scripts\release exists in a ' +
        'parent directory of this project, or set the WORKSPACE_TOOLS_ROOT ' +
        'environment variable to the TOOLS root.'
    )
}

if ($ToolsRoot) {
    if (-not [string]::IsNullOrWhiteSpace($Name)) {
        throw 'Use either -Name or -ToolsRoot, not both.'
    }
    return (Split-Path -Parent (Split-Path -Parent $releaseRoot))
}

if ([string]::IsNullOrWhiteSpace($Name)) { return $releaseRoot }

$helper = Join-Path $releaseRoot $Name
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    if ($Optional) { return $null }
    throw "Workspace release helper not found: $helper"
}
return $helper
