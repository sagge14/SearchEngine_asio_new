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

# 5. Production installer preserves ProgramData on update (SVC-005).
$installBat = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\Install-SearchEngineService-Windows7.bat'
Assert-True (Test-Path -LiteralPath $installBat) '5. Installer template exists'
$installText = [IO.File]::ReadAllText($installBat)
Assert-True (Test-StrictAsciiBytes -Path $installBat) '5. Installer template is strict ASCII'
Assert-True ($installText.Contains('DisableDelayedExpansion')) (
    '5. Installer keeps DisableDelayedExpansion'
)
Assert-True (-not $installText.Contains('ROLLBACK_DATA')) (
    '5. Installer has no whole-DataDir rollback variable'
)
Assert-True (-not $installText.Contains('move "%DATA_DIR%"')) (
    '5. Installer does not move DATA_DIR'
)
Assert-True (-not $installText.Contains('move "%DATA_DIR%" "%ROLLBACK_DATA%"')) (
    '5. Installer does not move DATA_DIR to ROLLBACK_DATA'
)
Assert-True (-not $installText.Contains('rmdir /S /Q "%DATA_DIR%"')) (
    '5. Reinstall rollback does not rmdir DATA_DIR'
)
Assert-True ($installText.Contains(':FRESH_DATA')) '5. Fresh data path is a separate label'
Assert-True ($installText.Contains(':UPDATE_DATA')) '5. Update data path is a separate label'
Assert-True ($installText.Contains('runtime-update-apply')) (
    '5. Update path calls runtime-update-apply'
)
Assert-True ($installText.Contains('runtime-update-commit')) (
    '5. Success path calls runtime-update-commit'
)
Assert-True ($installText.Contains('runtime-update-rollback')) (
    '5. Reinstall failure path calls runtime-update-rollback'
)
Assert-True ($installText.Contains(':WAIT_FOR_HEALTH_PORT')) (
    '5. Health helper takes an explicit port'
)
$updateBlock = $installText
$updateStart = $updateBlock.IndexOf("`n:UPDATE_DATA")
Assert-True ($updateStart -ge 0) '5. UPDATE_DATA label found for ignore check'
if ($updateStart -ge 0) {
    $updateSlice = $updateBlock.Substring($updateStart)
    $nextLabel = $updateSlice.IndexOf("`n:REGISTER_SERVICE")
    if ($nextLabel -lt 0) { $nextLabel = $updateSlice.Length }
    $updateOnly = $updateSlice.Substring(0, $nextLabel)
    Assert-True (-not $updateOnly.Contains('xcopy.exe "%PACKAGE_ROOT%data\*"')) (
        '5. Update path does not xcopy package data over DATA_DIR'
    )
    Assert-True (-not $updateOnly.ToLower().Contains('ignore.txt')) (
        '5. Update BAT path does not overwrite ignore.txt'
    )
}
Assert-True (-not $installText.Contains(
    'old settings, indexes and logs will be deleted'
)) '5. no-backup text does not claim ProgramData will be deleted'
Assert-True ($installText.Contains('Skipping the optional export does not delete ProgramData')) (
    '5. no-backup text says ProgramData is preserved'
)

$rollbackStart = $installText.IndexOf("`n:ROLLBACK_REINSTALL")
Assert-True ($rollbackStart -ge 0) '5. ROLLBACK_REINSTALL label found'
if ($rollbackStart -ge 0) {
    $rollbackSlice = $installText.Substring($rollbackStart)
    $posRollback = $rollbackSlice.IndexOf('runtime-update-rollback')
    $posOldHealth = $rollbackSlice.IndexOf('call :WAIT_FOR_HEALTH_PORT %OLD_port%')
    $posCommit = $rollbackSlice.IndexOf('runtime-update-commit')
    Assert-True ($posRollback -ge 0) '5. Rollback path calls runtime-update-rollback'
    Assert-True ($posOldHealth -ge 0) '5. Rollback path checks old-port health'
    Assert-True ($posCommit -ge 0) '5. Rollback path calls runtime-update-commit after restore'
    Assert-True (
        $posRollback -ge 0 -and $posOldHealth -ge 0 -and $posCommit -ge 0 -and
        $posRollback -lt $posOldHealth -and $posOldHealth -lt $posCommit
    ) '5. Rollback order is rollback then old-port health then commit'
}

# 6. SearchEngineConfig freshness considers RuntimeDataTransaction sources.
. (Join-Path $scriptsRoot 'Assert-SearchEngineConfigAutoPadContract.ps1')
$freshRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SearchEngineConfigFreshness-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path (Join-Path $freshRoot 'tools\config') | Out-Null
$fakeExe = Join-Path $freshRoot 'SearchEngineConfig.exe'
$fakeMain = Join-Path $freshRoot 'tools\config\main.cpp'
$fakeCpp = Join-Path $freshRoot 'tools\config\RuntimeDataTransaction.cpp'
$fakeH = Join-Path $freshRoot 'tools\config\RuntimeDataTransaction.h'
try {
    'exe' | Set-Content -LiteralPath $fakeExe -Encoding ASCII
    'main' | Set-Content -LiteralPath $fakeMain -Encoding ASCII
    'cpp' | Set-Content -LiteralPath $fakeCpp -Encoding ASCII
    'hdr' | Set-Content -LiteralPath $fakeH -Encoding ASCII
    $old = [DateTime]::UtcNow.AddHours(-2)
    $mid = [DateTime]::UtcNow.AddHours(-1)
    (Get-Item -LiteralPath $fakeExe).LastWriteTimeUtc = $mid
    (Get-Item -LiteralPath $fakeMain).LastWriteTimeUtc = $old
    (Get-Item -LiteralPath $fakeH).LastWriteTimeUtc = $old
    (Get-Item -LiteralPath $fakeCpp).LastWriteTimeUtc = $old
    try {
        Assert-SearchEngineConfigSourceFreshness -ConfigToolPath $fakeExe -ProjectRoot $freshRoot
        $script:passed++
        Write-Host 'PASS: 6. Fresh EXE newer than all config sources is accepted'
    } catch {
        $script:failures.Add("6. Fresh EXE newer than all config sources is accepted ($($_.Exception.Message))") | Out-Null
        Write-Host "FAIL: 6. Fresh EXE newer than all config sources is accepted"
    }

    (Get-Item -LiteralPath $fakeCpp).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(5)
    $threw = $false
    $errorText = ''
    try {
        Assert-SearchEngineConfigSourceFreshness -ConfigToolPath $fakeExe -ProjectRoot $freshRoot
    } catch {
        $threw = $true
        $errorText = [string]$_.Exception.Message
    }
    Assert-True (
        $threw -and $errorText.Contains('RuntimeDataTransaction.cpp')
    ) '6. Newer RuntimeDataTransaction.cpp makes SearchEngineConfig.exe stale'
} finally {
    if (Test-Path -LiteralPath $freshRoot) {
        Remove-Item -LiteralPath $freshRoot -Recurse -Force -ErrorAction SilentlyContinue
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
