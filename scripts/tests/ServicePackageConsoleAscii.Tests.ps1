# Regression tests for strict-ASCII portable console scripts.
# Does not install/uninstall Windows services or touch production configs.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptsRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent $scriptsRoot
. (Join-Path $scriptsRoot 'ConsoleScriptEncoding.ps1')

$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        $script:passed++
        Write-Host "PASS: $Message"
    } else {
        $script:failures.Add($Message) | Out-Null
        Write-Host "FAIL: $Message"
    }
}

function Assert-ThrowsContaining {
    param(
        [scriptblock]$Script,
        [string]$ExpectedSubstring,
        [string]$Message
    )
    $threw = $false
    $errorText = ''
    try {
        & $Script
    } catch {
        $threw = $true
        $errorText = [string]$_.Exception.Message
        if ([string]::IsNullOrWhiteSpace($errorText)) {
            $errorText = [string]$_
        }
    }
    if ($threw -and $errorText.Contains($ExpectedSubstring)) {
        $script:passed++
        Write-Host "PASS: $Message"
    } else {
        $detail = if ($threw) {
            "threw but message missing '$ExpectedSubstring': $errorText"
        } else {
            'did not throw'
        }
        $msg = "$Message ($detail)"
        $script:failures.Add($msg) | Out-Null
        Write-Host "FAIL: $msg"
    }
}

Write-Host '=== Service package console ASCII tests ==='

# 1. All service-package templates that are ASCII-rewritten are strict ASCII.
$templateRoots = @(
    (Join-Path $projectRoot 'deployment\BackupServicePortable'),
    (Join-Path $projectRoot 'deployment\SearchEngineServicePortable')
)
$templateScripts = @()
foreach ($root in $templateRoots) {
    $templateScripts += Get-ChildItem -LiteralPath $root -File |
        Where-Object { $_.Extension -in @('.bat', '.cmd') }
}
Assert-True ($templateScripts.Count -gt 0) '1. Found service-package .bat/.cmd templates'
foreach ($file in $templateScripts) {
    Assert-True (Test-StrictAsciiBytes -Path $file.FullName) (
        "1. Template is strict ASCII: $($file.FullName)"
    )
    $text = [IO.File]::ReadAllText($file.FullName)
    Assert-True (-not $text.Contains('???')) (
        "1. Template has no ??? replacement cluster: $($file.Name)"
    )
}

# 2. Write-StrictAsciiText accepts ASCII and rejects non-ASCII with full path.
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'ServicePackageConsoleAscii-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
    $asciiPath = Join-Path $tempRoot 'ok.bat'
    Write-StrictAsciiText -Path $asciiPath -Text "@echo off`r`necho hello`r`n"
    Assert-True (Test-StrictAsciiBytes -Path $asciiPath) '2. ASCII write produces strict ASCII bytes'
    Assert-True (-not ([IO.File]::ReadAllText($asciiPath)).Contains('???')) (
        '2. ASCII write has no ??? cluster'
    )

    $badPath = Join-Path $tempRoot 'bad.bat'
    $nonAscii = "@echo off`r`necho Stop gracefully - wait`r`necho Cyrillic: Отмена`r`n"
    Assert-ThrowsContaining `
        -Script { Write-StrictAsciiText -Path $badPath -Text $nonAscii } `
        -ExpectedSubstring $badPath `
        -Message '2. Non-ASCII write fails with full path'
    Assert-True (-not (Test-Path -LiteralPath $badPath)) (
        '2. Failed non-ASCII write does not leave a replaced file'
    )

    # 3. Emulate packager rewrite path: token replace then strict ASCII write.
    $emulatedPath = Join-Path $tempRoot 'Stop-BackupService.bat'
    $source = Join-Path $projectRoot `
        'deployment\BackupServicePortable\Stop-BackupService-Windows7.bat'
    Copy-Item -LiteralPath $source -Destination $emulatedPath
    $batchText = [IO.File]::ReadAllText($emulatedPath)
    $batchText = $batchText.Replace('{{ARCHITECTURE}}', 'x86-modern')
    $batchText = $batchText.Replace('{{VC_REDIST_FILE}}', 'vc_redist.x86.exe')
    $batchText = ($batchText -replace "`r?`n", "`r`n")
    Write-StrictAsciiText -Path $emulatedPath -Text $batchText
    $emulatedText = [IO.File]::ReadAllText($emulatedPath)
    Assert-True (Test-StrictAsciiBytes -Path $emulatedPath) (
        '3. Emulated packaged Stop-BackupService.bat is strict ASCII'
    )
    Assert-True ($emulatedText.Contains(
        '[1] Stop gracefully - wait for current tasks to finish (recommended)'
    )) '3. Packaged menu line [1] is readable ASCII English'
    Assert-True ($emulatedText.Contains(
        '[2] Stop immediately - interrupt the backup in progress'
    )) '3. Packaged menu line [2] is readable ASCII English'
    Assert-True ($emulatedText.Contains('[0] Cancel')) (
        '3. Packaged menu line [0] is readable ASCII English'
    )
    Assert-True (-not $emulatedText.Contains('???')) (
        '3. Emulated package has no ??? replacement cluster'
    )

    # 4. Intentional non-ASCII injection into rewrite path fails clearly.
    $injectPath = Join-Path $tempRoot 'Injected.bat'
    Copy-Item -LiteralPath $source -Destination $injectPath
    $injected = [IO.File]::ReadAllText($injectPath) + "`r`necho Отмена`r`n"
    $injected = ($injected -replace "`r?`n", "`r`n")
    Assert-ThrowsContaining `
        -Script { Write-StrictAsciiText -Path $injectPath -Text $injected } `
        -ExpectedSubstring 'Non-ASCII character' `
        -Message '4. Injected non-ASCII fails with clear error'
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host "Passed: $script:passed  Failed: $($script:failures.Count)"
if ($script:failures.Count -gt 0) {
    Write-Host 'Failures:'
    foreach ($failure in $script:failures) {
        Write-Host " - $failure"
    }
    exit 1
}
exit 0
