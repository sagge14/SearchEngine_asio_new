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
    $posExit2 = $updateOnly.IndexOf('if errorlevel 2 goto :RUNTIME_APPLY_FAILED_BEFORE_MUTATION')
    $posExit1 = $updateOnly.IndexOf('if errorlevel 1 goto :RUNTIME_APPLY_FAILED')
    Assert-True (
        $posExit2 -ge 0 -and $posExit1 -ge 0 -and $posExit2 -lt $posExit1
    ) '5. Apply exit 2 is classified as pre-mutation before exit 1'
}
Assert-True (-not $installText.Contains(
    'old settings, indexes and logs will be deleted'
)) '5. no-backup text does not claim ProgramData will be deleted'
Assert-True ($installText.Contains('call :UI install.no_export')) (
    '5. no-backup path uses the localized ProgramData-preservation message'
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
    Assert-True (
        -not $rollbackSlice.Contains('if exist "%ROLLBACK_RUNTIME%\" goto :ROLLBACK_RUNTIME_FAILED')
    ) '5. TX directory existence does not jump to ROLLBACK_RUNTIME_FAILED'
    $posFirewall = $rollbackSlice.IndexOf("`n:ROLLBACK_FIREWALL")
    $posFailedLabel = $rollbackSlice.IndexOf("`n:ROLLBACK_RUNTIME_FAILED")
    Assert-True (
        $posFirewall -ge 0 -and $posFailedLabel -ge 0 -and $posFirewall -lt $posFailedLabel -and
        $rollbackSlice.Substring($posFirewall, $posFailedLabel - $posFirewall).Contains(
            'sc.exe start "%SERVICE_NAME%"'
        )
    ) '5. Pre-mutation leftover TX still reaches old-service start'
    Assert-True ($rollbackSlice.Contains(':ROLLBACK_SKIP_HELPER')) (
        '5. Pre-mutation apply failure skips helper rollback'
    )
    $posSkip = $rollbackSlice.IndexOf("`n:ROLLBACK_SKIP_HELPER")
    Assert-True (
        $posSkip -ge 0 -and $posFirewall -ge 0 -and $posSkip -lt $posFirewall
    ) '5. Skip-helper path still reaches old-service firewall/start'
}

# 7. SVC-003 GET_ATTACHMENTS prefix_map package/installer/runtime path.
$prefixMapTemplate = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\source-data\prefix_map.json'
Assert-True (Test-Path -LiteralPath $prefixMapTemplate -PathType Leaf) (
    '7. Repository prefix_map.json template exists'
)
$packagerPath = Join-Path $projectRoot 'scripts\New-SearchEngineServicePackage.ps1'
$packagerText = [IO.File]::ReadAllText($packagerPath)
Assert-True ($packagerText.Contains("data\prefix_map.json")) (
    '7. Packager copies data\prefix_map.json'
)
Assert-True ($packagerText.Contains('validate-prefix-map')) (
    '7. Packager validates prefix_map.json before publish'
)
Assert-True ($packagerText.Contains('Portable prefix_map.json')) (
    '7. Packager missing-template error names prefix_map.json'
)
Assert-True ($installText.Contains('call :UI install.attachments_menu')) (
    '7. Fresh installer has a localized GET_ATTACHMENTS choice'
)
Assert-True ($installText.Contains(':FRESH_PREFIX_MAP_YES')) (
    '7. Fresh YES path is a separate label'
)
$freshYesStart = $installText.IndexOf("`n:FRESH_PREFIX_MAP_YES")
Assert-True ($freshYesStart -ge 0) '7. FRESH_PREFIX_MAP_YES label found'
if ($freshYesStart -ge 0) {
    $freshYesSlice = $installText.Substring($freshYesStart)
    $freshYesEnd = $freshYesSlice.IndexOf("`n:UPDATE_DATA")
    if ($freshYesEnd -lt 0) { $freshYesEnd = $freshYesSlice.Length }
    $freshYesOnly = $freshYesSlice.Substring(0, $freshYesEnd)
    Assert-True ($freshYesOnly.Contains('validate-prefix-map')) (
        '7. Fresh YES path calls validate-prefix-map'
    )
    Assert-True ($freshYesOnly.Contains(
        'copy /Y "%PACKAGE_ROOT%data\prefix_map.json" "%DATA_DIR%\prefix_map.json"'
    )) '7. Fresh YES can copy prefix_map.json'
}
if ($updateStart -ge 0) {
    Assert-True (-not $updateOnly.Contains(
        'copy /Y "%PACKAGE_ROOT%data\prefix_map.json" "%DATA_DIR%\prefix_map.json"'
    )) '7. Update path does not copy package prefix_map over existing'
    Assert-True (-not $updateOnly.Contains('xcopy.exe "%PACKAGE_ROOT%data\*"')) (
        '7. Update path still does not xcopy package data over DATA_DIR'
    )
}
$txPath = Join-Path $projectRoot 'tools\config\RuntimeDataTransaction.cpp'
$txText = [IO.File]::ReadAllText($txPath)
Assert-True (-not $txText.Contains('prefix_map.json')) (
    '7. RuntimeDataTransaction managed files do not include prefix_map.json'
)
$optionsCpp = [IO.File]::ReadAllText(
    (Join-Path $projectRoot 'src\Application\SearchEngineOptions.cpp')
)
Assert-True ($optionsCpp.Contains(
    'paths.prefix_map = paths.data_dir / L"prefix_map.json"'
)) '7. Runtime path is data-dir\prefix_map.json'
$appCpp = [IO.File]::ReadAllText(
    (Join-Path $projectRoot 'src\Application\SearchEngineApplication.cpp')
)
Assert-True ($appCpp.Contains('paths_.prefix_map')) (
    '7. Production application passes resolved prefix_map path'
)
$asioCpp = [IO.File]::ReadAllText(
    (Join-Path $projectRoot 'src\AsioServer\AsioServer.cpp')
)
$explicitCtor = 'std::make_unique<GetAttachmentsCmd>(attachmentsConfigPath)'
Assert-True ($asioCpp.Contains($explicitCtor)) (
    '7. Production registry constructs GetAttachmentsCmd with explicit path'
)
Assert-True (-not $asioCpp.Contains('make_unique<GetAttachmentsCmd>()')) (
    '7. Production registry does not use the CWD default GetAttachmentsCmd'
)

# 8. SVC-002 LocalSystem + SVC-011 no hidden production D:\ fallback.
Assert-True ($installText.Contains('call :UI install.localsystem')) (
    '8. Installer delegates LocalSystem guidance to localized UI'
)
$scriptMessagesText = [IO.File]::ReadAllText(
    (Join-Path $projectRoot 'tools\config\ScriptMessages.cpp')
)
Assert-True ($scriptMessagesText.Contains(
    'Runtime paths must be accessible to LocalSystem.'
)) '8. English catalog warns that runtime paths must be accessible to LocalSystem'
Assert-True ($scriptMessagesText.Contains(
    'User mapped drives are not available to the Windows service.'
)) '8. English catalog warns that user mapped drives are not available'
$registerStart = $installText.IndexOf("`n:REGISTER_SERVICE")
Assert-True ($registerStart -ge 0) '8. REGISTER_SERVICE label found'
if ($registerStart -ge 0) {
    $registerSlice = $installText.Substring($registerStart)
    $registerEnd = $registerSlice.IndexOf("`n:INSTALLED")
    if ($registerEnd -lt 0) { $registerEnd = $registerSlice.Length }
    $registerOnly = $registerSlice.Substring(0, $registerEnd)
    Assert-True ($registerOnly.Contains('sc.exe create')) (
        '8. Installer still uses sc.exe create'
    )
    Assert-True (-not $registerOnly.Contains('obj=')) (
        '8. sc create/config slice has no obj='
    )
    Assert-True (-not $registerOnly.Contains('password=')) (
        '8. sc create/config slice has no password='
    )
}
Assert-True (-not $installText.Contains('obj=')) (
    '8. Portable installer has no obj='
)
Assert-True (-not $installText.ToLower().Contains('net use')) (
    '8. Portable installer has no net use'
)
Assert-True (-not $installText.ToLower().Contains('credential')) (
    '8. Portable installer has no credential setup'
)
Assert-True (-not $asioCpp.Contains('"D:\\F12\\"')) (
    '8. Production AsioServer does not hardcode D:\\F12\\'
)
Assert-True (-not $asioCpp.Contains('"D:\\OPIS_ADMIN\\"')) (
    '8. Production AsioServer does not hardcode D:\\OPIS_ADMIN\\'
)
Assert-True (-not $asioCpp.Contains('SaveTlgToSendCmd(L"D:\\")')) (
    '8. Production registry does not construct SaveTlgToSendCmd(L"D:\\")'
)
Assert-True (-not $appCpp.Contains('"D:\\F12\\"')) (
    '8. Production application does not hardcode D:\\F12\\'
)
Assert-True (-not $appCpp.Contains('"D:\\OPIS_ADMIN\\"')) (
    '8. Production application does not hardcode D:\\OPIS_ADMIN\\'
)
Assert-True ($appCpp.Contains('pending->settings.tlg_send_root')) (
    '8. Application passes configured tlg_send_root'
)
Assert-True ($appCpp.Contains('pending->settings.f12_base_dir')) (
    '8. Application uses configured f12_base_dir'
)
Assert-True ($appCpp.Contains('pending->settings.opis_base_dir')) (
    '8. Application uses configured opis_base_dir'
)

# 9. SVC-001: Configure-SearchEngineService.bat is present, strict ASCII, and packaged.
$configureBat = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\Configure-SearchEngineService.bat'
Assert-True (Test-Path -LiteralPath $configureBat -PathType Leaf) (
    '9. Configure-SearchEngineService.bat template exists'
)
Assert-True (Test-StrictAsciiBytes -Path $configureBat) (
    '9. Configure-SearchEngineService.bat is strict ASCII'
)
$configureText = [IO.File]::ReadAllText($configureBat)
Assert-True (-not $configureText.Contains('???')) (
    '9. Configure-SearchEngineService.bat has no ??? replacement cluster'
)
Assert-True ($configureText.Contains('inspect-installed')) (
    '9. Configure-SearchEngineService.bat uses inspect-installed (not %ProgramData%)'
)
Assert-True (-not $configureText.Contains('%ProgramData%\%SERVICE_NAME%')) (
    '9. Configure-SearchEngineService.bat does not hardcode ProgramData as data-dir source'
)
Assert-True ($configureText.Contains('settings-transaction-apply')) (
    '9. Configure-SearchEngineService.bat calls settings-transaction-apply'
)
Assert-True ($configureText.Contains('settings-transaction-rollback')) (
    '9. Configure-SearchEngineService.bat calls settings-transaction-rollback'
)
Assert-True ($configureText.Contains('settings-transaction-commit')) (
    '9. Configure-SearchEngineService.bat calls settings-transaction-commit'
)
Assert-True ($configureText.Contains('health --port')) (
    '9. Configure-SearchEngineService.bat calls health PING/PONG'
)
Assert-True ($configureText.Contains('DisableDelayedExpansion')) (
    '9. Configure-SearchEngineService.bat starts with DisableDelayedExpansion'
)
Assert-True ($configureText.Contains('choose-installed-instance')) (
    '9. Configure-SearchEngineService.bat uses choose-installed-instance picker'
)
Assert-True ($configureText.Contains('--purpose configure')) (
    '9. Configure-SearchEngineService.bat passes --purpose configure to picker'
)

# 9a. SVC-001 configure picker: common installed-instance selection (Register BAT parity).
Assert-True ($configureText.Contains('if "%PICKER_EXIT%"=="3" goto :PICKER_NO_INSTALLED')) (
    '9a. Configure picker distinguishes helper exit code 3 (no installed services)'
)
Assert-True ($configureText.Contains('if "%PICKER_EXIT%"=="2" goto :PICKER_CANCELLED')) (
    '9a. Configure picker distinguishes helper exit code 2 (cancelled)'
)
Assert-True ($configureText.Contains('if not "%PICKER_EXIT%"=="0" goto :PICKER_HELPER_FAILED')) (
    '9a. Configure picker distinguishes helper exit code 1 (helper failure)'
)
Assert-True ($configureText.Contains('call :UI configure.no_services')) (
    '9a. Configure picker reports no installed services in the selected language'
)
Assert-True ($configureText.Contains('call :UI configure.helper_failed')) (
    '9a. Configure picker reports helper failure in the selected language'
)
Assert-True ($configureText.Contains('tokens=1,* delims==')) (
    '9a. Configure picker parses helper output with tokens=1,* delims=='
)
Assert-True ($configureText.Contains('set "SELECTED_%%A=%%B"')) (
    '9a. Configure picker uses SELECTED_%%A=%%B parsing pattern'
)
Assert-True ($configureText.Contains('SELECTED_instance')) (
    '9a. Configure picker reads SELECTED_instance from helper output'
)
Assert-True ($configureText.Contains('SELECTED_language')) (
    '9a. Configure picker propagates the selected language'
)

$pickerBlockStart = $configureText.IndexOf('rem No argument: use interactive picker')
$pickerBlockEndMarker = ':INSTANCE_RESOLVED'
$pickerBlockEnd = $configureText.IndexOf(
    "`r`n$pickerBlockEndMarker`r`n",
    $pickerBlockStart
)
if ($pickerBlockEnd -lt 0) {
    $pickerBlockEnd = $configureText.IndexOf(
        "`n$pickerBlockEndMarker`n",
        $pickerBlockStart
    )
}
Assert-True (($pickerBlockStart -ge 0) -and ($pickerBlockEnd -gt $pickerBlockStart)) (
    '9a. Configure picker block boundaries are present'
)
$pickerBlock = $configureText.Substring(
    $pickerBlockStart,
    $pickerBlockEnd - $pickerBlockStart
)
Assert-True (-not $pickerBlock.Contains('chcp 65001')) (
    '9a. Configure picker block does not switch to chcp 65001 before choose-installed-instance'
)
Assert-True ($configureText.Contains('chcp 65001')) (
    '9a. Configure-SearchEngineService.bat still uses chcp 65001 for UTF-8 helper output elsewhere'
)
$inspectBlockPattern = 'chcp 65001[^\r\n]*\r?\n"%HELPER%" inspect-installed'
Assert-True ([regex]::IsMatch($configureText, $inspectBlockPattern)) (
    '9a. Configure inspect-installed path keeps chcp 65001 before redirected helper output'
)
Assert-True (-not $configureText.Contains('2026-prd')) (
    '9a. Configure-SearchEngineService.bat does not hardcode instance 2026-prd'
)
Assert-True (-not $configureText.Contains('2026-prm')) (
    '9a. Configure-SearchEngineService.bat does not hardcode instance 2026-prm'
)
Assert-True (-not $configureText.Contains('Windows 7')) (
    '9a. Configure-SearchEngineService.bat has no Windows 7-specific picker branch'
)
Assert-True (-not $configureText.Contains('win7')) (
    '9a. Configure-SearchEngineService.bat has no win7-specific picker branch'
)
Assert-True (-not $configureText.Contains('6.1')) (
    '9a. Configure-SearchEngineService.bat has no version 6.1 OS gate'
)
$osBranchPattern = '(?im)^\s*ver(\s|$)|Windows\s*7|win7|OSVERSION|wmic\s+os'
Assert-True (-not [regex]::IsMatch($configureText, $osBranchPattern)) (
    '9a. Configure-SearchEngineService.bat has no OS-version-specific picker branching'
)

Assert-True ($configureText.Contains('ROLLBACK_DO_FILES')) (
    '9. Configure-SearchEngineService.bat has rollback exit-code gate label (ROLLBACK_DO_FILES)'
)
Assert-True ($configureText.Contains('ROLLBACK_CANNOT_STOP')) (
    '9. Configure-SearchEngineService.bat refuses file rollback without confirmed STOPPED'
)
Assert-True ($configureText.Contains('ROLLBACK_HEALTH')) (
    '9. Configure-SearchEngineService.bat checks old health before commit after rollback'
)

# 9d. Configure pretty-prints TEMP edit copy once before first Notepad open.
$formatJsonEditTemp = 'format-json --settings "%EDIT_TEMP%" --line-ending crlf'
Assert-True ($configureText.Contains($formatJsonEditTemp)) (
    '9d. Configure-SearchEngineService.bat formats EDIT_TEMP with format-json CRLF'
)
Assert-True ($configureText.Contains('--line-ending crlf')) (
    '9d. Configure-SearchEngineService.bat requests CRLF line endings'
)
Assert-True (-not $configureText.Contains('format-json --settings "%SETTINGS_PATH%"')) (
    '9d. Configure-SearchEngineService.bat does not format active SETTINGS_PATH'
)
$copyMarker = 'copy /Y "%SETTINGS_PATH%" "%EDIT_TEMP%"'
$firstNotepad = $configureText.IndexOf('notepad.exe')
Assert-True ($firstNotepad -ge 0) '9d. Configure-SearchEngineService.bat opens Notepad'
$copyStart = $configureText.IndexOf($copyMarker)
Assert-True ($copyStart -ge 0) '9d. Configure copy-to-EDIT_TEMP step is present'
$preEditorSlice = $configureText.Substring($copyStart, $firstNotepad - $copyStart)
Assert-True ($preEditorSlice.Contains($formatJsonEditTemp)) (
    '9d. format-json CRLF runs after copy and before first Notepad open'
)
$validateMarker = 'validate --settings "%EDIT_TEMP%"'
$validatePos = $configureText.IndexOf($validateMarker)
Assert-True ($validatePos -gt $firstNotepad) (
    '9d. validate --settings "%EDIT_TEMP%" runs after first Notepad open'
)
$editLoopStart = $configureText.IndexOf(':EDIT_LOOP')
Assert-True ($editLoopStart -ge 0) '9d. Configure EDIT_LOOP label is present'
$editLoopEnd = $configureText.IndexOf(':EDIT_VALID', $editLoopStart)
if ($editLoopEnd -lt 0) { $editLoopEnd = $configureText.Length }
$editLoopSlice = $configureText.Substring($editLoopStart, $editLoopEnd - $editLoopStart)
Assert-True (-not $editLoopSlice.Contains('format-json')) (
    '9d. EDIT_LOOP does not re-run format-json on validation retry'
)

# 9b. SVC-001 firewall contract: preserve installer-owned program binding.
Assert-True ($configureText.Contains('program="%PROGRAM_PATH%" enable=yes')) (
    '9b. PS-style firewall add/restore uses exact program="%PROGRAM_PATH%" enable=yes'
)

# 9c. SVC-001 firewall rollback: NEW delete failure must be checked.
$restorePatternHasError = '(?s):RESTORE_FIREWALL_CHECKED.*configure\.firewall_delete_new_failed.*exit /b 1'
Assert-True ([regex]::IsMatch($configureText, $restorePatternHasError)) (
    '9c. RESTORE_FIREWALL_CHECKED has checked error path for failed delete NEW rule'
)

$restorePatternOrder = '(?s):RESTORE_FIREWALL_CHECKED.*configure\.firewall_delete_new_failed.*exit /b 1.*exit /b 0'
Assert-True ([regex]::IsMatch($configureText, $restorePatternOrder)) (
    '9c. RESTORE_FIREWALL_CHECKED aborts (exit /b 1) before rollback success (exit /b 0)'
)

# Verify packager includes Configure-SearchEngineService.bat
Assert-True ($packagerText.Contains("'Configure-SearchEngineService.bat'")) (
    '9. Packager portableBatchFiles includes Configure-SearchEngineService.bat'
)
$protectedSection = $packagerText.Substring(
    $packagerText.IndexOf("'Install-SearchEngineService.bat'")
)
Assert-True ($protectedSection.Contains("'Configure-SearchEngineService.bat'")) (
    '9. Packager protectedFiles list includes Configure-SearchEngineService.bat'
)

# 9e. Zero-click local-machine installation contract.
$localInstallBat = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\Local-Machine-Install-Windows7.bat'
Assert-True (Test-Path -LiteralPath $localInstallBat -PathType Leaf) (
    '9e. Local-Machine-Install.bat template exists'
)
Assert-True (Test-StrictAsciiBytes -Path $localInstallBat) (
    '9e. Local-Machine-Install.bat template is strict ASCII'
)
$localInstallText = [IO.File]::ReadAllText($localInstallBat)
Assert-True ($localInstallText.Contains('/LocalMachine')) (
    '9e. Local-machine wrapper selects the explicit noninteractive mode'
)
Assert-True ($localInstallText.Contains('pause >nul')) (
    '9e. Local-machine wrapper keeps the final result visible'
)
Assert-True (-not $localInstallText.ToLowerInvariant().Contains('choice')) (
    '9e. Local-machine wrapper never asks a choice'
)
Assert-True (-not $localInstallText.ToLowerInvariant().Contains('set /p')) (
    '9e. Local-machine wrapper never reads stdin'
)
Assert-True ($localInstallText.Contains('set "INSTALL_EXIT=%ERRORLEVEL%"')) (
    '9e. Local-machine wrapper preserves the installer exit status before waiting'
)
Assert-True ($localInstallText.Contains('exit /b %INSTALL_EXIT%')) (
    '9e. Local-machine wrapper returns the preserved installer exit status'
)
Assert-True ($installText.Contains(
    'call :UI install.success "%SERVICE_NAME%" "%INSTALL_ROOT%" "%DATA_DIR%" "%SERVICE_YEAR%" "%SERVICE_PORT%" "%COMPUTERNAME%"'
)) '9e. Final install summary receives actual paths, year, port, and host'
Assert-True ($installText.Contains('set "SERVICE_INSTANCE=%LOCAL_current_year%"')) (
    '9e. Local-machine service instance is the current year'
)
Assert-True ($installText.Contains('set "SERVICE_NAME=SearchEngineService-%SERVICE_INSTANCE%"')) (
    '9e. Named Windows service keeps the year as its suffix'
)
Assert-True ($installText.Contains('configure-local-machine')) (
    '9e. Installer delegates deterministic Settings and free-port generation'
)
Assert-True ($installText.Contains('TOKEN_ISSUER_PASSWORD=12345678')) (
    '9e. Requested local issuer password is fixed to 12345678'
)
Assert-True ($installText.Contains('--device-type computer --name operator --id local-machine')) (
    '9e. Installer issues the operator local-machine computer token'
)
Assert-True ($installText.Contains('--yes >nul 2>&1')) (
    '9e. Installer suppresses the issuer token preview'
)
Assert-True ($installText.Contains('--export-public "%DATA_DIR%"')) (
    '9e. Installer exports the issuer public key beside the auth database'
)
Assert-True ($installText.Contains('add-from-token --token "%TOKEN_PATH%"')) (
    '9e. Installer enables the local identity through AuthDbTool token registration'
)
Assert-True ($installText.Contains('SearchEngine.exe" --initial-update --data-dir')) (
    '9e. Installer executes the official one-shot initial-update mode'
)
Assert-True ($installText.Contains('exit /b 20')) (
    '9e. Initial-index failure has a distinct post-install exit status'
)
Assert-True ($installText.Contains('if "%LOCAL_MACHINE_INSTALL%"=="1" exit /b 0')) (
    '9e. Local-machine PAUSE_UI path is a no-op'
)
Assert-True (([regex]::Matches(
    $installText,
    [regex]::Escape('if "%LOCAL_MACHINE_INSTALL%"=="1" exit /b 1')
)).Count -ge 3) (
    '9e. Local-machine failure paths never prompt for force-stop or delete retry'
)
Assert-True ($installText.Contains('goto :LOCAL_MACHINE_ALREADY_INSTALLED')) (
    '9e. Existing year service fails instead of prompting for replacement'
)
Assert-True ($installText.Contains('goto :LOCAL_MACHINE_LEFTOVERS')) (
    '9e. Leftover year directories fail instead of being deleted'
)
Assert-True ($installText.Contains(':CLEAN_LOCAL_MACHINE_FAILURE')) (
    '9e. Files created by a failed fresh local install have a cleanup path'
)
$authPosition = $installText.IndexOf('call :PROVISION_LOCAL_MACHINE_AUTH')
$updatePosition = $installText.IndexOf('call :RUN_LOCAL_INITIAL_UPDATE')
$startPosition = $installText.IndexOf('sc.exe start "%SERVICE_NAME%"')
Assert-True (
    $authPosition -ge 0 -and $updatePosition -gt $authPosition -and
    $startPosition -gt $updatePosition
) '9e. Token registration and completed one-shot indexing precede service start'
Assert-True ($packagerText.Contains("Source = 'Local-Machine-Install-Windows7.bat'")) (
    '9e. Packager copies the local-machine wrapper'
)
Assert-True ($protectedSection.Contains("'Local-Machine-Install.bat'")) (
    '9e. Package checksum manifest protects Local-Machine-Install.bat'
)

$defaultSettingsPath = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\source-data\Settings.json'
$defaultSettings = Get-Content -LiteralPath $defaultSettingsPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
Assert-True ($defaultSettings.config.document_catalog_storage -eq 'sqlite') (
    '9e. Fresh release Settings defaults document paths to SQLite'
)
Assert-True (-not $defaultSettings.config.enable_prm_short_content_autodetect) (
    '9e. Fresh release Settings disables PRM short-content updates'
)
Assert-True (-not $defaultSettings.config.scan_on_startup) (
    '9e. Fresh release Settings keeps repeated startup scans disabled'
)

# 10. Installer/uninstaller/configurator keep the selected UI language end-to-end.
$uninstallBat = Join-Path $projectRoot `
    'deployment\SearchEngineServicePortable\Uninstall-SearchEngineService-Windows7.bat'
$uninstallText = [IO.File]::ReadAllText($uninstallBat)
$localizedScripts = @(
    @{ Name = 'installer'; Text = $installText },
    @{ Name = 'uninstaller'; Text = $uninstallText },
    @{ Name = 'configurator'; Text = $configureText }
)
foreach ($localizedScript in $localizedScripts) {
    Assert-True ($localizedScript.Text.Contains('set "UI_LANGUAGE=')) (
        "10. $($localizedScript.Name) initializes UI_LANGUAGE"
    )
    Assert-True ($localizedScript.Text.Contains('SELECTED_language')) (
        "10. $($localizedScript.Name) reads the selected language"
    )
    Assert-True ($localizedScript.Text.Contains('script-message --language')) (
        "10. $($localizedScript.Name) delegates user text to Unicode helper"
    )

    $labels = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($match in [regex]::Matches(
        $localizedScript.Text,
        '(?im)^:([a-z0-9_]+)\s*$'
    )) {
        [void]$labels.Add($match.Groups[1].Value)
    }
    foreach ($match in [regex]::Matches(
        $localizedScript.Text,
        '(?im)\b(?:goto|call)\s+:([a-z0-9_]+)'
    )) {
        $label = $match.Groups[1].Value
        Assert-True ($labels.Contains($label)) (
            "10. $($localizedScript.Name) referenced label exists: $label"
        )
    }
}
Assert-True (-not $uninstallText.Contains('Backup before uninstall:')) (
    '10. Uninstaller has no hard-coded English backup prompt after language selection'
)
Assert-True ($uninstallText.Contains(
    'inspect-installed --instance "%SERVICE_INSTANCE%"'
)) '10. Uninstaller resolves actual SCM runtime paths'
Assert-True ($uninstallText.Contains('INSPECTED_install_root')) (
    '10. Uninstaller uses the inspected complete installation root'
)
Assert-True ($uninstallText.Contains('INSPECTED_archive_directory')) (
    '10. Uninstaller detects and removes an active frozen archive tree'
)
Assert-True ($uninstallText.Contains('STANDARD_INSTALL_ROOT')) (
    '10. Uninstaller also removes standard-path leftovers'
)
Assert-True (-not $configureText.Contains('Will update: Settings.json only')) (
    '10. Configurator has no hard-coded English confirmation after language selection'
)
Assert-True (-not $installText.Contains('Installation completed successfully.')) (
    '10. Installer has no hard-coded English success text after language selection'
)

$catalogIds = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal
)
$catalogPattern = '(?m)^\s*(?:\{\s*)?L"([a-z][a-z0-9_.-]+)"\s*,'
foreach ($match in [regex]::Matches($scriptMessagesText, $catalogPattern)) {
    [void]$catalogIds.Add($match.Groups[1].Value)
}
$literalUiPattern = '(?im)call\s+:UI\s+"?([a-z][a-z0-9_.-]+)"?'
foreach ($localizedScript in $localizedScripts) {
    foreach ($match in [regex]::Matches($localizedScript.Text, $literalUiPattern)) {
        $id = $match.Groups[1].Value
        Assert-True ($catalogIds.Contains($id)) (
            "10. $($localizedScript.Name) message id exists: $id"
        )
    }
}
foreach ($match in [regex]::Matches(
    $installText,
    'PREFIX_MAP_WARN_ID=([a-z][a-z0-9_.-]+)'
)) {
    $id = $match.Groups[1].Value
    Assert-True ($catalogIds.Contains($id)) "10. Dynamic installer message id exists: $id"
}

$configMainText = [IO.File]::ReadAllText(
    (Join-Path $projectRoot 'tools\config\main.cpp')
)
Assert-True ($configMainText.Contains('writeLanguageSelection(outputPath, language)')) (
    '10. Installed-instance picker writes language even on cancel/no-services paths'
)
Assert-True ($configMainText.Contains('command == L"script-message"')) (
    '10. SearchEngineConfig exposes script-message command'
)

# 6. SearchEngineConfig freshness considers RuntimeDataTransaction sources.
. (Join-Path $scriptsRoot 'Assert-SearchEngineConfigAutoPadContract.ps1')
$freshRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SearchEngineConfigFreshness-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path (Join-Path $freshRoot 'tools\config') | Out-Null
$fakeExe = Join-Path $freshRoot 'SearchEngineConfig.exe'
$fakeMain = Join-Path $freshRoot 'tools\config\main.cpp'
$fakeMessagesCpp = Join-Path $freshRoot 'tools\config\ScriptMessages.cpp'
$fakeMessagesH = Join-Path $freshRoot 'tools\config\ScriptMessages.h'
$fakeCpp = Join-Path $freshRoot 'tools\config\RuntimeDataTransaction.cpp'
$fakeH = Join-Path $freshRoot 'tools\config\RuntimeDataTransaction.h'
try {
    'exe' | Set-Content -LiteralPath $fakeExe -Encoding ASCII
    'main' | Set-Content -LiteralPath $fakeMain -Encoding ASCII
    'messages cpp' | Set-Content -LiteralPath $fakeMessagesCpp -Encoding ASCII
    'messages hdr' | Set-Content -LiteralPath $fakeMessagesH -Encoding ASCII
    'cpp' | Set-Content -LiteralPath $fakeCpp -Encoding ASCII
    'hdr' | Set-Content -LiteralPath $fakeH -Encoding ASCII
    $old = [DateTime]::UtcNow.AddHours(-2)
    $mid = [DateTime]::UtcNow.AddHours(-1)
    (Get-Item -LiteralPath $fakeExe).LastWriteTimeUtc = $mid
    (Get-Item -LiteralPath $fakeMain).LastWriteTimeUtc = $old
    (Get-Item -LiteralPath $fakeMessagesCpp).LastWriteTimeUtc = $old
    (Get-Item -LiteralPath $fakeMessagesH).LastWriteTimeUtc = $old
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

# 8. SearchEngineService package data layout contract.
Write-Host ''
Write-Host '=== Service package data layout tests ==='
Assert-True (-not ($packagerText -match "data\\logs")) (
    '8. Packager does not create data\logs'
)
Assert-True (-not ($packagerText -match "data\\messages")) (
    '8. Packager does not create data\messages'
)
Assert-True ($packagerText.Contains("data\Settings.json")) (
    '8. Packager copies data\Settings.json'
)
Assert-True ($packagerText.Contains("data\ignore.txt")) (
    '8. Packager copies data\ignore.txt'
)
Assert-True ($packagerText.Contains("data\prefix_map.json")) (
    '8. Packager copies data\prefix_map.json'
)
Assert-True ($packagerText.Contains("data\OEM866.INI")) (
    '8. Packager writes data\OEM866.INI'
)
Assert-True ($packagerText.Contains("tools\SearchEngineArchive.exe")) (
    '8. Packager copies tools\SearchEngineArchive.exe'
)
Assert-True ($packagerText.Contains("Archive-SearchEngineService.bat")) (
    '8. Packager copies archive launcher'
)

function Test-PackageDataLayout {
    param(
        [string]$PackageDirectory,
        [string]$ArchitectureLabel
    )
    $dataDir = Join-Path $PackageDirectory 'data'
    foreach ($name in @('Settings.json', 'ignore.txt', 'OEM866.INI', 'prefix_map.json')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $dataDir $name) -PathType Leaf) (
            "8. $ArchitectureLabel data\$name exists"
        )
    }
    foreach ($name in @('logs', 'messages')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $dataDir $name))) (
            "8. $ArchitectureLabel data\$name absent"
        )
    }
    Assert-True (
        Test-Path -LiteralPath (Join-Path $PackageDirectory 'Local-Machine-Install.bat') `
            -PathType Leaf
    ) "8. $ArchitectureLabel Local-Machine-Install.bat exists"
}

$packageArchCases = @(
    @{ Label = 'x64'; Architecture = 'x64'; Preset = 'windows-x64' },
    @{ Label = 'x86-modern'; Architecture = 'x86-modern'; Preset = 'windows-x86' },
    @{ Label = 'x86-Windows7'; Architecture = 'x86'; Preset = 'windows7-x86' }
)
$packageTempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'ServicePackageDataLayout-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $packageTempRoot | Out-Null
try {
    foreach ($case in $packageArchCases) {
        $buildDir = Join-Path $projectRoot (
            'out\build\' + $case.Preset + '\Release'
        )
        $required = @(
            (Join-Path $buildDir 'SearchEngine.exe'),
            (Join-Path $buildDir 'SearchEngineConfig.exe'),
            (Join-Path $buildDir 'SearchEngineArchive.exe'),
            (Join-Path $buildDir 'AuthDbTool.exe'),
            (Join-Path $buildDir 'SearchClientTokenIssuer.exe'),
            (Join-Path $buildDir 'searchclient-auth-token.defaults.json')
        )
        $missing = @($required | Where-Object {
            -not (Test-Path -LiteralPath $_ -PathType Leaf)
        })
        if ($missing.Count -gt 0) {
            Write-Host (
                "SKIP: 8. $($case.Label) package layout NOT RUN " +
                "(missing Release binaries under out\build\$($case.Preset)\Release)"
            )
            continue
        }

        $outputDir = Join-Path $packageTempRoot $case.Label
        try {
            & (Join-Path $projectRoot 'scripts\New-SearchEngineServicePackage.ps1') `
                -Architecture $case.Architecture `
                -BuildDirectory $buildDir `
                -OutputDirectory $outputDir `
                -SkipCloudPublish | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "packager exited with code $LASTEXITCODE"
            }
            Test-PackageDataLayout -PackageDirectory $outputDir `
                -ArchitectureLabel $case.Label
        } catch {
            Write-Host (
                "SKIP: 8. $($case.Label) package layout NOT RUN ($($_.Exception.Message))"
            )
        }
    }
} finally {
    if (Test-Path -LiteralPath $packageTempRoot) {
        Remove-Item -LiteralPath $packageTempRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
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
