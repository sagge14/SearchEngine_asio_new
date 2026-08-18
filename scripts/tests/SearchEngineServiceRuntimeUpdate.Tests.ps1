# Filesystem tests for SearchEngineConfig runtime-update apply/rollback/commit.
# Does not install Windows services or touch production ProgramData.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$HelperPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $HelperPath -PathType Leaf)) {
    Write-Host "FAIL: SearchEngineConfig.exe was not found: $HelperPath"
    exit 1
}

$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0
$script:tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SearchEngineRuntimeUpdate-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $script:tempRoot | Out-Null

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

function Get-FileRecord {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force
    $bytes = [IO.File]::ReadAllBytes($Path)
    $head = if ($bytes.Length -ge 16) { $bytes[0..15] } else { $bytes }
    $tail = if ($bytes.Length -ge 16) {
        $bytes[($bytes.Length - 16)..($bytes.Length - 1)]
    } else {
        $bytes
    }
    return [pscustomobject]@{
        Length = $item.Length
        LastWriteTimeUtc = $item.LastWriteTimeUtc
        Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        Head = [Convert]::ToBase64String($head)
        Tail = [Convert]::ToBase64String($tail)
    }
}

function Assert-FileUnchanged {
    param(
        [string]$Path,
        $Before,
        [string]$Message
    )
    $after = Get-FileRecord -Path $Path
    Assert-True (
        $after.Length -eq $Before.Length -and
        $after.Sha256 -eq $Before.Sha256 -and
        $after.LastWriteTimeUtc -eq $Before.LastWriteTimeUtc
    ) $Message
}

function New-TextFile {
    param([string]$Path, [string]$Text)
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir | Out-Null
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}

function New-RollbackDir {
    param([string]$DataDir)
    $parent = Split-Path -Parent $DataDir
    $name = Split-Path -Leaf $DataDir
    $suffix = '{0}-{1}' -f (Get-Random -Maximum 99999999), (Get-Random -Maximum 99999999)
    return Join-Path $parent ($name + '.runtime-update-' + $suffix)
}

function New-PackageData {
    param([string]$Root, [string]$Oem, [string]$Ignore)
    $package = Join-Path $Root 'package-data'
    New-Item -ItemType Directory -Path $package | Out-Null
    New-TextFile -Path (Join-Path $package 'OEM866.INI') -Text $Oem
    New-TextFile -Path (Join-Path $package 'ignore.txt') -Text $Ignore
    return $package
}

function New-SentinelDataDir {
    param(
        [string]$DataDir,
        [switch]$OmitSettings,
        [switch]$OmitOem,
        [switch]$OmitEndpoint,
        [switch]$OmitIgnore,
        [switch]$OmitLogs,
        [switch]$OmitMessages,
        [switch]$LargeIndex
    )
    New-Item -ItemType Directory -Path $DataDir | Out-Null
    if (-not $OmitLogs) {
        New-Item -ItemType Directory -Path (Join-Path $DataDir 'logs') | Out-Null
        New-TextFile -Path (Join-Path $DataDir 'logs\existing.log') -Text 'old-log-sentinel'
    }
    if (-not $OmitMessages) {
        New-Item -ItemType Directory -Path (Join-Path $DataDir 'messages') | Out-Null
        New-TextFile -Path (Join-Path $DataDir 'messages\message.bin') -Text 'old-message-sentinel'
    }
    New-Item -ItemType Directory -Path (Join-Path $DataDir 'operator-added') | Out-Null
    New-TextFile -Path (Join-Path $DataDir 'auth_clients.sqlite') -Text 'auth-db-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'issuer-public.pem') -Text 'issuer-pem-sentinel'
    if ($LargeIndex) {
        $indexPath = Join-Path $DataDir 'inverted_index.sqlite'
        $stream = [IO.File]::Open($indexPath, 'Create', 'Write', 'None')
        try {
            $head = [Text.Encoding]::ASCII.GetBytes('HEAD-SENTINEL-INDEX')
            $tail = [Text.Encoding]::ASCII.GetBytes('TAIL-SENTINEL-INDEX')
            $stream.Write($head, 0, $head.Length)
            $stream.SetLength(16MB)
            $stream.Seek(-$tail.Length, 'End') | Out-Null
            $stream.Write($tail, 0, $tail.Length)
        } finally {
            $stream.Dispose()
        }
    } else {
        New-TextFile -Path (Join-Path $DataDir 'inverted_index.sqlite') -Text 'index-sentinel'
    }
    New-TextFile -Path (Join-Path $DataDir 'inverted_index.sqlite-wal') -Text 'wal-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'inverted_index.sqlite-shm') -Text 'shm-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'prefix_map.json') -Text '{"prefix":"old"}'
    if (-not $OmitIgnore) {
        New-TextFile -Path (Join-Path $DataDir 'ignore.txt') -Text 'user-ignore-sentinel'
    }
    New-TextFile -Path (Join-Path $DataDir 'log.db') -Text 'logdb-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'server_log.log') -Text 'server-log-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'unknown.txt') -Text 'unknown-runtime-sentinel'
    New-TextFile -Path (Join-Path $DataDir 'operator-added\custom.bin') -Text 'operator-bin-sentinel'
    if (-not $OmitSettings) {
        New-TextFile -Path (Join-Path $DataDir 'Settings.json') -Text '{"old":"settings"}'
    }
    if (-not $OmitOem) {
        New-TextFile -Path (Join-Path $DataDir 'OEM866.INI') -Text 'old-oem-sentinel'
    }
    if (-not $OmitEndpoint) {
        New-TextFile -Path (Join-Path $DataDir 'client-endpoint.txt') -Text "port=1111`r`n"
    }
}

function Get-PersistentRecords {
    param([string]$DataDir)
    $names = @(
        'auth_clients.sqlite',
        'issuer-public.pem',
        'inverted_index.sqlite',
        'inverted_index.sqlite-wal',
        'inverted_index.sqlite-shm',
        'messages\message.bin',
        'prefix_map.json',
        'ignore.txt',
        'log.db',
        'server_log.log',
        'logs\existing.log',
        'unknown.txt',
        'operator-added\custom.bin'
    )
    $map = @{}
    foreach ($name in $names) {
        $path = Join-Path $DataDir $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $map[$name] = Get-FileRecord -Path $path
        }
    }
    return $map
}

function Assert-PersistentUnchanged {
    param([string]$DataDir, $Before, [string]$Prefix)
    $after = Get-PersistentRecords -DataDir $DataDir
    foreach ($name in $Before.Keys) {
        Assert-True ($after.ContainsKey($name)) "$Prefix persistent still exists: $name"
        if ($after.ContainsKey($name)) {
            Assert-FileUnchanged -Path (Join-Path $DataDir $name) -Before $Before[$name] (
                "$Prefix persistent bytes unchanged: $name"
            )
        }
    }
}

function Assert-TransactionSmall {
    param([string]$RollbackDir, [string]$DataDir, [string]$Prefix)
    Assert-True (Test-Path -LiteralPath $RollbackDir -PathType Container) (
        "$Prefix rollback directory exists"
    )
    $forbidden = @(
        'auth_clients.sqlite',
        'issuer-public.pem',
        'inverted_index.sqlite',
        'inverted_index.sqlite-wal',
        'inverted_index.sqlite-shm',
        'prefix_map.json',
        'ignore.txt',
        'log.db',
        'server_log.log',
        'unknown.txt',
        'messages',
        'logs'
    )
    $files = Get-ChildItem -LiteralPath $RollbackDir -Force
    foreach ($item in $files) {
        Assert-True ($item.Name -notin $forbidden) (
            "$Prefix rollback allows $($item.Name)"
        )
        Assert-True (-not $item.PSIsContainer) (
            "$Prefix rollback contains only files: $($item.Name)"
        )
    }
    $names = @($files | ForEach-Object { $_.Name })
    Assert-True ($names -contains '.searchengine-runtime-update-marker') (
        "$Prefix rollback contains marker"
    )
    Assert-True ($names -contains '.searchengine-runtime-update-phase') (
        "$Prefix rollback contains phase"
    )
    Assert-True ($names -contains 'manifest.json') (
        "$Prefix rollback contains manifest"
    )
    $total = ($files | Measure-Object -Property Length -Sum).Sum
    $index = Get-Item -LiteralPath (Join-Path $DataDir 'inverted_index.sqlite')
    Assert-True ($total -lt [Math]::Max(1MB, ($index.Length / 4))) (
        "$Prefix transaction directory is much smaller than the index"
    )
}

function Invoke-RuntimeCommand {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    $allArgs = @($Command) + $Arguments
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $HelperPath @allArgs 2>&1 | ForEach-Object { $_.ToString() } | Out-String
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = $output
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-TransactionPhase {
    param([string]$RollbackDir)
    $phasePath = Join-Path $RollbackDir '.searchengine-runtime-update-phase'
    if (-not (Test-Path -LiteralPath $phasePath)) {
        return ''
    }
    return ([string](Get-Content -LiteralPath $phasePath -Raw)).Trim()
}

try {
    Write-Host '=== SearchEngine runtime-update tests ==='

    $oldSettings = '{"old":"settings"}'
    $newSettings = '{"new":"settings-from-template"}'
    $oldOem = 'old-oem-sentinel'
    $newOem = 'new-oem-from-package'
    $oldEndpoint = "port=1111`r`n"
    $newEndpoint = "server_id=default`r`nport=2222`r`n"
    $userIgnore = 'user-ignore-sentinel'
    $packageIgnore = 'package-default-ignore'

    # 10.1 Successful update + commit
    $case1 = Join-Path $script:tempRoot 'case-success'
    New-Item -ItemType Directory -Path $case1 | Out-Null
    $data1 = Join-Path $case1 'SearchEngineService'
    New-SentinelDataDir -DataDir $data1 -LargeIndex
    $package1 = New-PackageData -Root $case1 -Oem $newOem -Ignore $packageIgnore
    $genSettings1 = Join-Path $case1 'generated-settings.json'
    $genEndpoint1 = Join-Path $case1 'generated-endpoint.txt'
    New-TextFile -Path $genSettings1 -Text $newSettings
    New-TextFile -Path $genEndpoint1 -Text $newEndpoint
    $before1 = Get-PersistentRecords -DataDir $data1
    $rb1 = New-RollbackDir -DataDir $data1
    $apply1 = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data1,
        '--package-data', $package1,
        '--generated-settings', $genSettings1,
        '--generated-endpoint', $genEndpoint1,
        '--rollback-dir', $rb1
    )
    Assert-True ($apply1.ExitCode -eq 0) '10.1 apply succeeded'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data1 'Settings.json') -Raw) -eq $newSettings) (
        '10.1 Settings.json updated'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data1 'OEM866.INI') -Raw) -eq $newOem) (
        '10.1 OEM866.INI updated'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data1 'client-endpoint.txt') -Raw) -eq $newEndpoint) (
        '10.1 client-endpoint.txt updated'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data1 'ignore.txt') -Raw) -eq $userIgnore) (
        '10.1 existing ignore.txt unchanged'
    )
    Assert-PersistentUnchanged -DataDir $data1 -Before $before1 -Prefix '10.1'
    Assert-TransactionSmall -RollbackDir $rb1 -DataDir $data1 -Prefix '10.1'
    $commit1 = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $data1,
        '--rollback-dir', $rb1
    )
    Assert-True ($commit1.ExitCode -eq 0) '10.1 commit succeeded'
    Assert-True (-not (Test-Path -LiteralPath $rb1)) '10.1 transaction directory removed'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data1 'Settings.json') -Raw) -eq $newSettings) (
        '10.1 Settings remain after commit'
    )
    Assert-PersistentUnchanged -DataDir $data1 -Before $before1 -Prefix '10.1 after commit'

    # 10.2 Rollback restores managed files
    $case2 = Join-Path $script:tempRoot 'case-rollback'
    New-Item -ItemType Directory -Path $case2 | Out-Null
    $data2 = Join-Path $case2 'SearchEngineService'
    New-SentinelDataDir -DataDir $data2
    $package2 = New-PackageData -Root $case2 -Oem $newOem -Ignore $packageIgnore
    $genSettings2 = Join-Path $case2 'generated-settings.json'
    $genEndpoint2 = Join-Path $case2 'generated-endpoint.txt'
    New-TextFile -Path $genSettings2 -Text $newSettings
    New-TextFile -Path $genEndpoint2 -Text $newEndpoint
    $before2 = Get-PersistentRecords -DataDir $data2
    $rb2 = New-RollbackDir -DataDir $data2
    $apply2 = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data2,
        '--package-data', $package2,
        '--generated-settings', $genSettings2,
        '--generated-endpoint', $genEndpoint2,
        '--rollback-dir', $rb2
    )
    Assert-True ($apply2.ExitCode -eq 0) '10.2 apply succeeded'
    $rollback2 = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data2,
        '--rollback-dir', $rb2
    )
    Assert-True ($rollback2.ExitCode -eq 0) '10.2 rollback succeeded'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data2 'Settings.json') -Raw) -eq $oldSettings) (
        '10.2 Settings.json restored'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data2 'OEM866.INI') -Raw) -eq $oldOem) (
        '10.2 OEM866.INI restored'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data2 'client-endpoint.txt') -Raw) -eq $oldEndpoint) (
        '10.2 client-endpoint.txt restored'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data2 'ignore.txt') -Raw) -eq $userIgnore) (
        '10.2 existing ignore.txt unchanged after rollback'
    )
    Assert-PersistentUnchanged -DataDir $data2 -Before $before2 -Prefix '10.2'
    Assert-True (Test-Path -LiteralPath $rb2) (
        '10.2 transaction directory remains after rollback'
    )
    $commit2 = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $data2,
        '--rollback-dir', $rb2
    )
    Assert-True ($commit2.ExitCode -eq 0) '10.2 commit after rollback succeeded'
    Assert-True (-not (Test-Path -LiteralPath $rb2)) (
        '10.2 transaction directory removed after explicit commit'
    )

    # 10.3 Missing managed files
    $case3 = Join-Path $script:tempRoot 'case-missing'
    New-Item -ItemType Directory -Path $case3 | Out-Null
    $data3 = Join-Path $case3 'SearchEngineService'
    New-SentinelDataDir -DataDir $data3 -OmitSettings -OmitOem -OmitEndpoint -OmitIgnore -OmitLogs -OmitMessages
    $package3 = New-PackageData -Root $case3 -Oem $newOem -Ignore $packageIgnore
    $genSettings3 = Join-Path $case3 'generated-settings.json'
    $genEndpoint3 = Join-Path $case3 'generated-endpoint.txt'
    New-TextFile -Path $genSettings3 -Text $newSettings
    New-TextFile -Path $genEndpoint3 -Text $newEndpoint
    $rb3 = New-RollbackDir -DataDir $data3
    $apply3 = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data3,
        '--package-data', $package3,
        '--generated-settings', $genSettings3,
        '--generated-endpoint', $genEndpoint3,
        '--rollback-dir', $rb3
    )
    Assert-True ($apply3.ExitCode -eq 0) '10.3 apply created missing managed files'
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'Settings.json')) '10.3 Settings.json created'
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'OEM866.INI')) '10.3 OEM866.INI created'
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'client-endpoint.txt')) '10.3 endpoint created'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data3 'ignore.txt') -Raw) -eq $packageIgnore) (
        '10.3 default ignore created only because it was missing'
    )
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'logs') -PathType Container) '10.3 logs created'
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'messages') -PathType Container) '10.3 messages created'
    New-TextFile -Path (Join-Path $data3 'logs\new-during-update.log') -Text 'keep-me'
    New-TextFile -Path (Join-Path $data3 'messages\new-during-update.bin') -Text 'keep-me-too'
    $rollback3 = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data3,
        '--rollback-dir', $rb3
    )
    Assert-True ($rollback3.ExitCode -eq 0) '10.3 rollback succeeded'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $data3 'Settings.json'))) (
        '10.3 rollback removed new Settings.json'
    )
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $data3 'OEM866.INI'))) (
        '10.3 rollback removed new OEM866.INI'
    )
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $data3 'client-endpoint.txt'))) (
        '10.3 rollback removed new endpoint'
    )
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $data3 'ignore.txt'))) (
        '10.3 rollback removed installer-created ignore.txt'
    )
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'logs\new-during-update.log')) (
        '10.3 rollback kept new logs'
    )
    Assert-True (Test-Path -LiteralPath (Join-Path $data3 'messages\new-during-update.bin')) (
        '10.3 rollback kept new messages'
    )
    Assert-True (Test-Path -LiteralPath $rb3) '10.3 transaction remains after rollback'
    $null = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $data3,
        '--rollback-dir', $rb3
    )

    $case3b = Join-Path $script:tempRoot 'case-ignore-changed'
    New-Item -ItemType Directory -Path $case3b | Out-Null
    $data3b = Join-Path $case3b 'SearchEngineService'
    New-SentinelDataDir -DataDir $data3b -OmitIgnore
    $package3b = New-PackageData -Root $case3b -Oem $newOem -Ignore $packageIgnore
    $genSettings3b = Join-Path $case3b 'generated-settings.json'
    $genEndpoint3b = Join-Path $case3b 'generated-endpoint.txt'
    New-TextFile -Path $genSettings3b -Text $newSettings
    New-TextFile -Path $genEndpoint3b -Text $newEndpoint
    $rb3b = New-RollbackDir -DataDir $data3b
    $null = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data3b,
        '--package-data', $package3b,
        '--generated-settings', $genSettings3b,
        '--generated-endpoint', $genEndpoint3b,
        '--rollback-dir', $rb3b
    )
    New-TextFile -Path (Join-Path $data3b 'ignore.txt') -Text 'externally-changed-ignore'
    $rollback3b = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data3b,
        '--rollback-dir', $rb3b
    )
    Assert-True ($rollback3b.ExitCode -eq 0) '10.3 changed-ignore rollback still succeeds'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data3b 'ignore.txt') -Raw) -eq 'externally-changed-ignore') (
        '10.3 changed installer ignore is kept'
    )
    Assert-True ($rollback3b.Output -match 'WARNING:') (
        '10.3 changed installer ignore emits a warning'
    )

    # 10.4 Paths with spaces, Cyrillic, and two instances
    $cyrillicName = -join @(
        [char]0x041F, [char]0x0443, [char]0x0442, [char]0x044C,
        ' ',
        [char]0x0441,
        ' ',
        [char]0x043F, [char]0x0440, [char]0x043E, [char]0x0431,
        [char]0x0435, [char]0x043B, [char]0x0430, [char]0x043C, [char]0x0438
    )
    $case4 = Join-Path $script:tempRoot $cyrillicName
    New-Item -ItemType Directory -Path $case4 | Out-Null
    $data4a = Join-Path $case4 'SearchEngineService-alpha'
    $data4b = Join-Path $case4 'SearchEngineService-beta'
    New-SentinelDataDir -DataDir $data4a
    New-SentinelDataDir -DataDir $data4b
    New-TextFile -Path (Join-Path $data4b 'unknown.txt') -Text 'beta-unknown'
    $package4 = New-PackageData -Root $case4 -Oem $newOem -Ignore $packageIgnore
    $genSettings4 = Join-Path $case4 'generated-settings.json'
    $genEndpoint4 = Join-Path $case4 'generated-endpoint.txt'
    New-TextFile -Path $genSettings4 -Text $newSettings
    New-TextFile -Path $genEndpoint4 -Text $newEndpoint
    $before4b = Get-PersistentRecords -DataDir $data4b
    $rb4a = New-RollbackDir -DataDir $data4a
    $apply4 = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data4a,
        '--package-data', $package4,
        '--generated-settings', $genSettings4,
        '--generated-endpoint', $genEndpoint4,
        '--rollback-dir', $rb4a
    )
    Assert-True ($apply4.ExitCode -eq 0) '10.4 apply on spaced/Cyrillic path succeeded'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data4a 'Settings.json') -Raw) -eq $newSettings) (
        '10.4 instance A Settings updated'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data4b 'Settings.json') -Raw) -eq $oldSettings) (
        '10.4 instance B Settings untouched'
    )
    Assert-PersistentUnchanged -DataDir $data4b -Before $before4b -Prefix '10.4 instance B'
    $null = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $data4a,
        '--rollback-dir', $rb4a
    )

    # 10.5 Large-file invariant already covered by case-success LargeIndex.
    Assert-True ($true) '10.5 large index sentinel was used in 10.1'

    # 10.6 Negative input before mutation
    $case6 = Join-Path $script:tempRoot 'case-negative'
    New-Item -ItemType Directory -Path $case6 | Out-Null
    $data6 = Join-Path $case6 'SearchEngineService'
    New-SentinelDataDir -DataDir $data6
    $package6 = New-PackageData -Root $case6 -Oem $newOem -Ignore $packageIgnore
    $genSettings6 = Join-Path $case6 'generated-settings.json'
    $genEndpoint6 = Join-Path $case6 'generated-endpoint.txt'
    New-TextFile -Path $genSettings6 -Text $newSettings
    New-TextFile -Path $genEndpoint6 -Text $newEndpoint
    $before6 = Get-PersistentRecords -DataDir $data6
    $settingsBefore6 = Get-FileRecord -Path (Join-Path $data6 'Settings.json')

    function Assert-NoMutation {
        param($Result, [string]$Message)
        Assert-True ($Result.ExitCode -ne 0) "$Message returns non-zero"
        Assert-FileUnchanged -Path (Join-Path $data6 'Settings.json') -Before $settingsBefore6 (
            "$Message left Settings.json unchanged"
        )
        Assert-PersistentUnchanged -DataDir $data6 -Before $before6 -Prefix $Message
    }

    $missingSettings = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', (Join-Path $case6 'no-settings.json'),
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', (New-RollbackDir -DataDir $data6)
    )
    Assert-NoMutation -Result $missingSettings -Message '10.6 missing generated Settings'

    $settingsDir = Join-Path $case6 'settings-as-dir'
    New-Item -ItemType Directory -Path $settingsDir | Out-Null
    $settingsIsDir = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $settingsDir,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', (New-RollbackDir -DataDir $data6)
    )
    Assert-NoMutation -Result $settingsIsDir -Message '10.6 Settings source is a directory'

    $oemDir = Join-Path $case6 'oem-package'
    New-Item -ItemType Directory -Path $oemDir | Out-Null
    New-TextFile -Path (Join-Path $oemDir 'ignore.txt') -Text $packageIgnore
    $missingOem = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $oemDir,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', (New-RollbackDir -DataDir $data6)
    )
    Assert-NoMutation -Result $missingOem -Message '10.6 missing package OEM'

    $endpointDir = Join-Path $case6 'endpoint-as-dir'
    New-Item -ItemType Directory -Path $endpointDir | Out-Null
    $endpointIsDir = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $endpointDir,
        '--rollback-dir', (New-RollbackDir -DataDir $data6)
    )
    Assert-NoMutation -Result $endpointIsDir -Message '10.6 endpoint source is a directory'

    $missingData = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', (Join-Path $case6 'missing-data'),
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', (Join-Path $case6 'missing-data.runtime-update-1-2')
    )
    Assert-True ($missingData.ExitCode -ne 0) '10.6 missing DATA_DIR returns non-zero'

    $dataAsFile = Join-Path $case6 'data-as-file'
    New-TextFile -Path $dataAsFile -Text 'not-a-dir'
    $dataIsFile = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $dataAsFile,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', (Join-Path $case6 'data-as-file.runtime-update-1-2')
    )
    Assert-True ($dataIsFile.ExitCode -ne 0) '10.6 DATA_DIR as file returns non-zero'

    $rbExists = New-RollbackDir -DataDir $data6
    New-Item -ItemType Directory -Path $rbExists | Out-Null
    $alreadyExists = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $rbExists
    )
    Assert-NoMutation -Result $alreadyExists -Message '10.6 rollback directory already exists'

    $inside = Join-Path $data6 'nested.runtime-update-1-2'
    $insideResult = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $inside
    )
    Assert-NoMutation -Result $insideResult -Message '10.6 rollback directory inside DATA_DIR'
    Assert-True (-not (Test-Path -LiteralPath $inside)) '10.6 nested rollback dir was not created'

    $badPrefix = Join-Path (Split-Path -Parent $data6) 'wrong-prefix-1-2'
    $badPrefixResult = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $badPrefix
    )
    Assert-NoMutation -Result $badPrefixResult -Message '10.6 rollback directory has wrong prefix'

    $foreignData = Join-Path $case6 'SearchEngineService-other'
    New-SentinelDataDir -DataDir $foreignData
    $rbForeign = New-RollbackDir -DataDir $data6
    $null = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $rbForeign
    )
    $wrongDirRollback = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $foreignData,
        '--rollback-dir', $rbForeign
    )
    Assert-True ($wrongDirRollback.ExitCode -ne 0) '10.6 rollback with other DATA_DIR fails'
    Assert-True (Test-Path -LiteralPath $rbForeign) '10.6 mismatched rollback keeps transaction directory'
    $null = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data6,
        '--rollback-dir', $rbForeign
    )

    $rbCorrupt = New-RollbackDir -DataDir $data6
    $null = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $rbCorrupt
    )
    New-TextFile -Path (Join-Path $rbCorrupt 'manifest.json') -Text 'not-json'
    $corrupt = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data6,
        '--rollback-dir', $rbCorrupt
    )
    Assert-True ($corrupt.ExitCode -ne 0) '10.6 damaged manifest fails closed'
    Assert-True (Test-Path -LiteralPath $rbCorrupt) '10.6 damaged manifest keeps transaction directory'

    $rbVersion = New-RollbackDir -DataDir $data6
    $null = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data6,
        '--package-data', $package6,
        '--generated-settings', $genSettings6,
        '--generated-endpoint', $genEndpoint6,
        '--rollback-dir', $rbVersion
    )
    $manifestPath = Join-Path $rbVersion 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.format_version = 99
    ($manifest | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $unknownVersion = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data6,
        '--rollback-dir', $rbVersion
    )
    Assert-True ($unknownVersion.ExitCode -ne 0) '10.6 unknown format_version fails closed'
    Assert-True (Test-Path -LiteralPath $rbVersion) '10.6 unknown version keeps transaction directory'

    # Restore case6 managed files for later? not needed.

    # 10.7 Partial apply failure
    $case7 = Join-Path $script:tempRoot 'case-partial'
    New-Item -ItemType Directory -Path $case7 | Out-Null
    $data7 = Join-Path $case7 'SearchEngineService'
    New-SentinelDataDir -DataDir $data7
    $package7 = New-PackageData -Root $case7 -Oem $newOem -Ignore $packageIgnore
    $genSettings7 = Join-Path $case7 'generated-settings.json'
    $genEndpoint7 = Join-Path $case7 'generated-endpoint.txt'
    New-TextFile -Path $genSettings7 -Text $newSettings
    New-TextFile -Path $genEndpoint7 -Text $newEndpoint
    $before7 = Get-PersistentRecords -DataDir $data7
    $rb7 = New-RollbackDir -DataDir $data7
    $endpointLock = [IO.File]::Open(
        (Join-Path $data7 'client-endpoint.txt'),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read
    )
    try {
        $partial = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
            '--data-dir', $data7,
            '--package-data', $package7,
            '--generated-settings', $genSettings7,
            '--generated-endpoint', $genEndpoint7,
            '--rollback-dir', $rb7
        )
    } finally {
        $endpointLock.Dispose()
    }
    Assert-True ($partial.ExitCode -ne 0) '10.7 partial apply returns non-zero'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data7 'Settings.json') -Raw) -eq $oldSettings) (
        '10.7 Settings.json restored by internal rollback'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data7 'OEM866.INI') -Raw) -eq $oldOem) (
        '10.7 OEM866.INI unchanged or restored'
    )
    Assert-PersistentUnchanged -DataDir $data7 -Before $before7 -Prefix '10.7'
    Assert-True (Test-Path -LiteralPath $rb7) '10.7 TX dir remains after partial apply'
    $phase7 = Get-TransactionPhase -RollbackDir $rb7
    if ($partial.Output -match 'rollback incomplete') {
        Assert-True ($phase7 -eq 'mutation_started') (
            '10.7 incomplete internal rollback keeps mutation_started'
        )
    } else {
        Assert-True ($phase7 -eq 'restored') (
            '10.7 successful internal rollback records restored'
        )
    }

    # 10.8 Rollback failure and retry
    $case8 = Join-Path $script:tempRoot 'case-retry'
    New-Item -ItemType Directory -Path $case8 | Out-Null
    $data8 = Join-Path $case8 'SearchEngineService'
    New-SentinelDataDir -DataDir $data8
    $package8 = New-PackageData -Root $case8 -Oem $newOem -Ignore $packageIgnore
    $genSettings8 = Join-Path $case8 'generated-settings.json'
    $genEndpoint8 = Join-Path $case8 'generated-endpoint.txt'
    New-TextFile -Path $genSettings8 -Text $newSettings
    New-TextFile -Path $genEndpoint8 -Text $newEndpoint
    $rb8 = New-RollbackDir -DataDir $data8
    $apply8 = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $data8,
        '--package-data', $package8,
        '--generated-settings', $genSettings8,
        '--generated-endpoint', $genEndpoint8,
        '--rollback-dir', $rb8
    )
    Assert-True ($apply8.ExitCode -eq 0) '10.8 apply succeeded'
    $settingsLock = [IO.File]::Open(
        (Join-Path $data8 'Settings.json'),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::None
    )
    try {
        $blocked = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
            '--data-dir', $data8,
            '--rollback-dir', $rb8
        )
        Assert-True ($blocked.ExitCode -ne 0) '10.8 blocked rollback returns non-zero'
        Assert-True (Test-Path -LiteralPath $rb8) '10.8 blocked rollback keeps TX dir'
        Assert-True ((Get-TransactionPhase -RollbackDir $rb8) -eq 'mutation_started') (
            '10.8 blocked rollback keeps mutation_started'
        )
        Assert-True (Test-Path -LiteralPath (Join-Path $rb8 'manifest.json')) (
            '10.8 commit was not performed automatically'
        )
    } finally {
        $settingsLock.Dispose()
    }
    $retry = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $data8,
        '--rollback-dir', $rb8
    )
    Assert-True ($retry.ExitCode -eq 0) '10.8 retry rollback succeeded'
    Assert-True ((Get-Content -LiteralPath (Join-Path $data8 'Settings.json') -Raw) -eq $oldSettings) (
        '10.8 Settings.json restored after retry'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $data8 'OEM866.INI') -Raw) -eq $oldOem) (
        '10.8 OEM866.INI restored after retry'
    )
    Assert-True (Test-Path -LiteralPath $rb8) '10.8 TX dir remains after successful rollback'
    Assert-True ((Get-TransactionPhase -RollbackDir $rb8) -eq 'restored') (
        '10.8 retry rollback records restored'
    )
    $commit8 = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $data8,
        '--rollback-dir', $rb8
    )
    Assert-True ($commit8.ExitCode -eq 0) '10.8 explicit commit after rollback succeeded'
    Assert-True (-not (Test-Path -LiteralPath $rb8)) '10.8 TX dir removed after explicit commit'

    # 10.9 Commit ownership
    $case9 = Join-Path $script:tempRoot 'case-commit'
    New-Item -ItemType Directory -Path $case9 | Out-Null
    $data9 = Join-Path $case9 'SearchEngineService'
    New-SentinelDataDir -DataDir $data9
    $package9 = New-PackageData -Root $case9 -Oem $newOem -Ignore $packageIgnore
    $genSettings9 = Join-Path $case9 'generated-settings.json'
    $genEndpoint9 = Join-Path $case9 'generated-endpoint.txt'
    New-TextFile -Path $genSettings9 -Text $newSettings
    New-TextFile -Path $genEndpoint9 -Text $newEndpoint

    function New-AppliedTx {
        param([string]$Suffix)
        $dir = Join-Path $case9 $Suffix
        New-Item -ItemType Directory -Path $dir | Out-Null
        $data = Join-Path $dir 'SearchEngineService'
        New-SentinelDataDir -DataDir $data
        $rb = New-RollbackDir -DataDir $data
        $result = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
            '--data-dir', $data,
            '--package-data', $package9,
            '--generated-settings', $genSettings9,
            '--generated-endpoint', $genEndpoint9,
            '--rollback-dir', $rb
        )
        Assert-True ($result.ExitCode -eq 0) "10.9 apply for $Suffix succeeded"
        return [pscustomobject]@{ DataDir = $data; RollbackDir = $rb }
    }

    $wrong = New-AppliedTx -Suffix 'wrong-dir'
    $other = Join-Path $case9 'other-data'
    New-SentinelDataDir -DataDir $other
    $commitWrong = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $other,
        '--rollback-dir', $wrong.RollbackDir
    )
    Assert-True ($commitWrong.ExitCode -ne 0) '10.9 commit with wrong DATA_DIR fails'
    Assert-True (Test-Path -LiteralPath $wrong.RollbackDir) '10.9 wrong DATA_DIR keeps TX dir'

    $noMarker = New-AppliedTx -Suffix 'no-marker'
    Remove-Item -LiteralPath (Join-Path $noMarker.RollbackDir '.searchengine-runtime-update-marker') -Force
    $commitNoMarker = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $noMarker.DataDir,
        '--rollback-dir', $noMarker.RollbackDir
    )
    Assert-True ($commitNoMarker.ExitCode -ne 0) '10.9 commit without marker fails'
    Assert-True (Test-Path -LiteralPath $noMarker.RollbackDir) '10.9 missing marker keeps TX dir'

    $badManifest = New-AppliedTx -Suffix 'bad-manifest'
    New-TextFile -Path (Join-Path $badManifest.RollbackDir 'manifest.json') -Text 'broken'
    $commitBadManifest = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $badManifest.DataDir,
        '--rollback-dir', $badManifest.RollbackDir
    )
    Assert-True ($commitBadManifest.ExitCode -ne 0) '10.9 commit with damaged manifest fails'
    Assert-True (Test-Path -LiteralPath $badManifest.RollbackDir) '10.9 damaged manifest keeps TX dir'

    $unexpected = New-AppliedTx -Suffix 'unexpected'
    New-TextFile -Path (Join-Path $unexpected.RollbackDir 'extra.txt') -Text 'nope'
    $commitUnexpected = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $unexpected.DataDir,
        '--rollback-dir', $unexpected.RollbackDir
    )
    Assert-True ($commitUnexpected.ExitCode -ne 0) '10.9 commit with unexpected file fails'
    Assert-True (Test-Path -LiteralPath $unexpected.RollbackDir) '10.9 unexpected file keeps TX dir'

    $reparseRoot = Join-Path $case9 'reparse'
    New-Item -ItemType Directory -Path $reparseRoot | Out-Null
    $reparseTarget = Join-Path $reparseRoot 'target'
    New-Item -ItemType Directory -Path $reparseTarget | Out-Null
    $reparseLink = Join-Path $reparseRoot 'link'
    $createdReparse = $false
    try {
        cmd.exe /c "mklink /J `"$reparseLink`" `"$reparseTarget`"" | Out-Null
        $createdReparse = (Test-Path -LiteralPath $reparseLink)
    } catch {
        $createdReparse = $false
    }
    if ($createdReparse) {
        $reparseTx = New-AppliedTx -Suffix 'reparse-tx'
        $attrs = [IO.File]::GetAttributes($reparseLink)
        Assert-True ($true) '10.9 reparse point creation is available'
        $null = $reparseTx
        $null = $attrs
        # A reparse inside the TX directory should block commit.
        $junctionInTx = Join-Path $reparseTx.RollbackDir 'odd-junction'
        cmd.exe /c "mklink /J `"$junctionInTx`" `"$reparseTarget`"" | Out-Null
        $commitReparse = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
            '--data-dir', $reparseTx.DataDir,
            '--rollback-dir', $reparseTx.RollbackDir
        )
        Assert-True ($commitReparse.ExitCode -ne 0) '10.9 commit with reparse point fails'
        Assert-True (Test-Path -LiteralPath $reparseTx.RollbackDir) '10.9 reparse commit keeps TX dir'
    } else {
        Assert-True ($true) '10.9 reparse point creation not available; skipped'
    }

    # Path validation for rollback/commit: wrong prefix and non-sibling.
    $casePath = Join-Path $script:tempRoot 'case-path-commands'
    New-Item -ItemType Directory -Path $casePath | Out-Null
    $dataPath = Join-Path $casePath 'SearchEngineService'
    New-SentinelDataDir -DataDir $dataPath
    $packagePath = New-PackageData -Root $casePath -Oem $newOem -Ignore $packageIgnore
    $genSettingsPath = Join-Path $casePath 'generated-settings.json'
    $genEndpointPath = Join-Path $casePath 'generated-endpoint.txt'
    New-TextFile -Path $genSettingsPath -Text $newSettings
    New-TextFile -Path $genEndpointPath -Text $newEndpoint
    $rbPath = New-RollbackDir -DataDir $dataPath
    $applyPath = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $dataPath,
        '--package-data', $packagePath,
        '--generated-settings', $genSettingsPath,
        '--generated-endpoint', $genEndpointPath,
        '--rollback-dir', $rbPath
    )
    Assert-True ($applyPath.ExitCode -eq 0) 'path-validation apply succeeded'
    $settingsAfterApply = Get-FileRecord -Path (Join-Path $dataPath 'Settings.json')
    $beforePath = Get-PersistentRecords -DataDir $dataPath

    function Copy-TransactionFiles {
        param([string]$Source, [string]$Destination)
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
        Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $Destination $_.Name)
        }
    }

    $badPrefixDir = Join-Path $casePath 'wrong-prefix-1-2'
    Copy-TransactionFiles -Source $rbPath -Destination $badPrefixDir
    $rollbackBadPrefix = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $badPrefixDir
    )
    $commitBadPrefix = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $badPrefixDir
    )
    Assert-True ($rollbackBadPrefix.ExitCode -ne 0) 'rollback with wrong prefix fails'
    Assert-True ($commitBadPrefix.ExitCode -ne 0) 'commit with wrong prefix fails'
    Assert-True (Test-Path -LiteralPath $badPrefixDir) 'wrong-prefix directory was not deleted'
    Assert-True (Test-Path -LiteralPath $rbPath) 'real transaction was not deleted by wrong prefix'
    Assert-FileUnchanged -Path (Join-Path $dataPath 'Settings.json') -Before $settingsAfterApply (
        'wrong-prefix rollback/commit left Settings.json unchanged'
    )
    Assert-PersistentUnchanged -DataDir $dataPath -Before $beforePath -Prefix 'wrong-prefix'

    $otherParent = Join-Path $casePath 'other-parent'
    New-Item -ItemType Directory -Path $otherParent | Out-Null
    $nonSibling = Join-Path $otherParent 'SearchEngineService.runtime-update-1-2'
    Copy-TransactionFiles -Source $rbPath -Destination $nonSibling
    $rollbackNonSibling = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $nonSibling
    )
    $commitNonSibling = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $nonSibling
    )
    Assert-True ($rollbackNonSibling.ExitCode -ne 0) 'rollback with non-sibling TX fails'
    Assert-True ($commitNonSibling.ExitCode -ne 0) 'commit with non-sibling TX fails'
    Assert-True (Test-Path -LiteralPath $nonSibling) 'non-sibling directory was not deleted'
    Assert-FileUnchanged -Path (Join-Path $dataPath 'Settings.json') -Before $settingsAfterApply (
        'non-sibling rollback/commit left Settings.json unchanged'
    )
    Assert-PersistentUnchanged -DataDir $dataPath -Before $beforePath -Prefix 'non-sibling'

    $rollbackKeep = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $rbPath
    )
    Assert-True ($rollbackKeep.ExitCode -eq 0) 'helper rollback restores files'
    Assert-True ((Get-Content -LiteralPath (Join-Path $dataPath 'Settings.json') -Raw) -eq $oldSettings) (
        'helper rollback restored Settings.json'
    )
    Assert-True (Test-Path -LiteralPath $rbPath) 'helper rollback keeps transaction until commit'
    $commitKeep = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $dataPath,
        '--rollback-dir', $rbPath
    )
    Assert-True ($commitKeep.ExitCode -eq 0) 'helper commit deletes transaction after rollback'
    Assert-True (-not (Test-Path -LiteralPath $rbPath)) 'transaction removed only after commit'

    if (-not ('NativeProc' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeProc {
    [DllImport("ntdll.dll")]
    public static extern int NtSuspendProcess(IntPtr processHandle);
    [DllImport("ntdll.dll")]
    public static extern int NtResumeProcess(IntPtr processHandle);
}
'@
    }

    # TEST A: pre-mutation apply failure keeps diagnostic TX with prepared phase.
    $caseA = Join-Path $script:tempRoot 'case-phase-prepared'
    New-Item -ItemType Directory -Path $caseA | Out-Null
    $dataA = Join-Path $caseA 'SearchEngineService'
    New-SentinelDataDir -DataDir $dataA
    Remove-Item -LiteralPath (Join-Path $dataA 'ignore.txt') -Force
    New-Item -ItemType Directory -Path (Join-Path $dataA 'ignore.txt') | Out-Null
    $settingsA = Get-FileRecord -Path (Join-Path $dataA 'Settings.json')
    $oemA = Get-FileRecord -Path (Join-Path $dataA 'OEM866.INI')
    $endpointA = Get-FileRecord -Path (Join-Path $dataA 'client-endpoint.txt')
    $beforeA = Get-PersistentRecords -DataDir $dataA
    $packageA = New-PackageData -Root $caseA -Oem $newOem -Ignore $packageIgnore
    $genSettingsA = Join-Path $caseA 'generated-settings.json'
    $genEndpointA = Join-Path $caseA 'generated-endpoint.txt'
    New-TextFile -Path $genSettingsA -Text $newSettings
    New-TextFile -Path $genEndpointA -Text $newEndpoint
    $rbA = New-RollbackDir -DataDir $dataA
    $applyA = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $dataA,
        '--package-data', $packageA,
        '--generated-settings', $genSettingsA,
        '--generated-endpoint', $genEndpointA,
        '--rollback-dir', $rbA
    )
    Assert-True ($applyA.ExitCode -ne 0) 'TEST A apply failed before mutation'
    Assert-True ($applyA.Output -match 'before managed mutation') (
        'TEST A reports pre-mutation failure'
    )
    Assert-True (Test-Path -LiteralPath $rbA) 'TEST A diagnostic TX directory was preserved'
    Assert-True ((Get-TransactionPhase -RollbackDir $rbA) -eq 'prepared') (
        'TEST A durable phase is prepared'
    )
    $rollbackA = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataA,
        '--rollback-dir', $rbA
    )
    Assert-True ($rollbackA.ExitCode -eq 0) 'TEST A rollback is not required'
    Assert-True ($rollbackA.Output -match 'rollback not required') (
        'TEST A helper reports rollback not required'
    )
    Assert-FileUnchanged -Path (Join-Path $dataA 'Settings.json') -Before $settingsA (
        'TEST A Settings.json unchanged'
    )
    Assert-FileUnchanged -Path (Join-Path $dataA 'OEM866.INI') -Before $oemA (
        'TEST A OEM866.INI unchanged'
    )
    Assert-FileUnchanged -Path (Join-Path $dataA 'client-endpoint.txt') -Before $endpointA (
        'TEST A client-endpoint.txt unchanged'
    )
    Assert-PersistentUnchanged -DataDir $dataA -Before $beforeA -Prefix 'TEST A'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $rbA 'Settings.json.snapshot'))) (
        'TEST A rollback did not require snapshots'
    )
    $commitA = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $dataA,
        '--rollback-dir', $rbA
    )
    Assert-True ($commitA.ExitCode -eq 0) 'TEST A commit cleans prepared TX'
    Assert-True (-not (Test-Path -LiteralPath $rbA)) 'TEST A TX removed after commit'

    # TEST B: mutation started, internal restore succeeded, TX stays restored.
    $caseB = Join-Path $script:tempRoot 'case-phase-restored'
    New-Item -ItemType Directory -Path $caseB | Out-Null
    $dataB = Join-Path $caseB 'SearchEngineService'
    New-SentinelDataDir -DataDir $dataB -OmitMessages
    New-TextFile -Path (Join-Path $dataB 'messages') -Text 'messages-as-file'
    $beforeB = Get-PersistentRecords -DataDir $dataB
    $packageB = New-PackageData -Root $caseB -Oem $newOem -Ignore $packageIgnore
    $genSettingsB = Join-Path $caseB 'generated-settings.json'
    $genEndpointB = Join-Path $caseB 'generated-endpoint.txt'
    New-TextFile -Path $genSettingsB -Text $newSettings
    New-TextFile -Path $genEndpointB -Text $newEndpoint
    $rbB = New-RollbackDir -DataDir $dataB
    $applyB = Invoke-RuntimeCommand -Command 'runtime-update-apply' -Arguments @(
        '--data-dir', $dataB,
        '--package-data', $packageB,
        '--generated-settings', $genSettingsB,
        '--generated-endpoint', $genEndpointB,
        '--rollback-dir', $rbB
    )
    Assert-True ($applyB.ExitCode -ne 0) 'TEST B apply failed after mutation'
    Assert-True ($applyB.Output -match 'managed files were rolled back') (
        'TEST B internal rollback succeeded'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $dataB 'Settings.json') -Raw) -eq $oldSettings) (
        'TEST B Settings.json restored'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $dataB 'OEM866.INI') -Raw) -eq $oldOem) (
        'TEST B OEM866.INI restored'
    )
    Assert-True ((Get-Content -LiteralPath (Join-Path $dataB 'client-endpoint.txt') -Raw) -eq $oldEndpoint) (
        'TEST B client-endpoint.txt restored'
    )
    Assert-PersistentUnchanged -DataDir $dataB -Before $beforeB -Prefix 'TEST B'
    Assert-True (Test-Path -LiteralPath $rbB) 'TEST B TX remains after internal restore'
    Assert-True ((Get-TransactionPhase -RollbackDir $rbB) -eq 'restored') (
        'TEST B durable phase is restored'
    )
    $rollbackB = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataB,
        '--rollback-dir', $rbB
    )
    Assert-True ($rollbackB.ExitCode -eq 0) 'TEST B repeat rollback is safe'
    Assert-True ($rollbackB.Output -match 'rollback not required') (
        'TEST B repeat rollback is a no-op'
    )
    Assert-True (Test-Path -LiteralPath $rbB) 'TEST B TX remains until explicit commit'
    $commitB = Invoke-RuntimeCommand -Command 'runtime-update-commit' -Arguments @(
        '--data-dir', $dataB,
        '--rollback-dir', $rbB
    )
    Assert-True ($commitB.ExitCode -eq 0) 'TEST B commit deletes restored TX'
    Assert-True (-not (Test-Path -LiteralPath $rbB)) 'TEST B TX removed after commit'

    # Ownership race: foreign file with a known snapshot basename is not owned.
    $caseOwn = Join-Path $script:tempRoot 'case-owned-race'
    New-Item -ItemType Directory -Path $caseOwn | Out-Null
    $dataOwn = Join-Path $caseOwn 'SearchEngineService'
    New-SentinelDataDir -DataDir $dataOwn
    $settingsOwn = Get-FileRecord -Path (Join-Path $dataOwn 'Settings.json')
    $beforeOwn = Get-PersistentRecords -DataDir $dataOwn
    $packageOwn = New-PackageData -Root $caseOwn -Oem $newOem -Ignore $packageIgnore
    $genSettingsOwn = Join-Path $caseOwn 'generated-settings.json'
    $genEndpointOwn = Join-Path $caseOwn 'generated-endpoint.txt'
    New-TextFile -Path $genSettingsOwn -Text $newSettings
    New-TextFile -Path $genEndpointOwn -Text $newEndpoint
    $rbOwn = New-RollbackDir -DataDir $dataOwn
    $foreignSnapshot = Join-Path $rbOwn 'Settings.json.snapshot'
    $foreignBytes = [Text.Encoding]::ASCII.GetBytes(
        'FOREIGN-SNAPSHOT-' + [Guid]::NewGuid().ToString('N')
    )
    $helperFull = (Resolve-Path -LiteralPath $HelperPath).Path
    $stdoutOwn = Join-Path $caseOwn 'apply.out'
    $stderrOwn = Join-Path $caseOwn 'apply.err'
    $psiOwn = New-Object System.Diagnostics.ProcessStartInfo
    $psiOwn.FileName = $helperFull
    $psiOwn.Arguments = @(
        'runtime-update-apply',
        '--data-dir', ('"{0}"' -f $dataOwn),
        '--package-data', ('"{0}"' -f $packageOwn),
        '--generated-settings', ('"{0}"' -f $genSettingsOwn),
        '--generated-endpoint', ('"{0}"' -f $genEndpointOwn),
        '--rollback-dir', ('"{0}"' -f $rbOwn)
    ) -join ' '
    $psiOwn.UseShellExecute = $false
    $psiOwn.RedirectStandardOutput = $true
    $psiOwn.RedirectStandardError = $true
    $psiOwn.CreateNoWindow = $true
    $procOwn = New-Object System.Diagnostics.Process
    $procOwn.StartInfo = $psiOwn
    [void]$procOwn.Start()
    $suspendedOwn = $false
    $applyOwnExit = -1
    try {
        $deadlineOwn = (Get-Date).AddSeconds(20)
        while (-not $procOwn.HasExited -and (Get-Date) -lt $deadlineOwn) {
            if (Test-Path -LiteralPath $rbOwn) {
                [void][NativeProc]::NtSuspendProcess($procOwn.Handle)
                $suspendedOwn = $true
                [IO.File]::WriteAllBytes($foreignSnapshot, $foreignBytes)
                break
            }
            Start-Sleep -Milliseconds 1
        }
        if ($suspendedOwn) {
            [void][NativeProc]::NtResumeProcess($procOwn.Handle)
            $suspendedOwn = $false
        }
        if (-not $procOwn.WaitForExit(30000)) {
            $procOwn.Kill()
            [void]$procOwn.WaitForExit(5000)
        }
        $applyOwnExit = $procOwn.ExitCode
        $procOwn.StandardOutput.ReadToEnd() | Set-Content -LiteralPath $stdoutOwn -Encoding ASCII
        $procOwn.StandardError.ReadToEnd() | Set-Content -LiteralPath $stderrOwn -Encoding ASCII
    } finally {
        if ($suspendedOwn -and -not $procOwn.HasExited) {
            [void][NativeProc]::NtResumeProcess($procOwn.Handle)
        }
        if (-not $procOwn.HasExited) {
            $procOwn.Kill()
        }
        $procOwn.Dispose()
    }
    Assert-True ($applyOwnExit -ne 0) 'ownership race apply failed'
    Assert-True (Test-Path -LiteralPath $foreignSnapshot) 'ownership race foreign snapshot remains'
    $afterForeign = [IO.File]::ReadAllBytes($foreignSnapshot)
    Assert-True (
        [Convert]::ToBase64String($afterForeign) -eq [Convert]::ToBase64String($foreignBytes)
    ) 'ownership race foreign snapshot bytes unchanged'
    Assert-FileUnchanged -Path (Join-Path $dataOwn 'Settings.json') -Before $settingsOwn (
        'ownership race Settings.json unchanged'
    )
    Assert-PersistentUnchanged -DataDir $dataOwn -Before $beforeOwn -Prefix 'ownership race'
    $rollbackOwn = Invoke-RuntimeCommand -Command 'runtime-update-rollback' -Arguments @(
        '--data-dir', $dataOwn,
        '--rollback-dir', $rbOwn
    )
    Assert-True ($rollbackOwn.ExitCode -eq 0) 'ownership race rollback not required'
    Assert-True ($rollbackOwn.Output -match 'rollback not required') (
        'ownership race helper does not restore from the foreign snapshot'
    )
    $afterForeign2 = [IO.File]::ReadAllBytes($foreignSnapshot)
    Assert-True (
        [Convert]::ToBase64String($afterForeign2) -eq [Convert]::ToBase64String($foreignBytes)
    ) 'ownership race foreign snapshot survived rollback'

    # Pre-mutation cleanup must not delete unexpected files.
    $caseIntruder = Join-Path $script:tempRoot 'case-intruder'
    New-Item -ItemType Directory -Path $caseIntruder | Out-Null
    $dataIntruder = Join-Path $caseIntruder 'SearchEngineService'
    New-SentinelDataDir -DataDir $dataIntruder
    Remove-Item -LiteralPath (Join-Path $dataIntruder 'ignore.txt') -Force
    New-Item -ItemType Directory -Path (Join-Path $dataIntruder 'ignore.txt') | Out-Null
    $bigSettings = Join-Path $dataIntruder 'Settings.json'
    $payload = New-Object byte[] (8MB)
    [IO.File]::WriteAllBytes($bigSettings, $payload)
    $packageIntruder = New-PackageData -Root $caseIntruder -Oem $newOem -Ignore $packageIgnore
    $genSettingsIntruder = Join-Path $caseIntruder 'generated-settings.json'
    $genEndpointIntruder = Join-Path $caseIntruder 'generated-endpoint.txt'
    New-TextFile -Path $genSettingsIntruder -Text $newSettings
    New-TextFile -Path $genEndpointIntruder -Text $newEndpoint
    $rbIntruder = New-RollbackDir -DataDir $dataIntruder
    $intruderFile = Join-Path $rbIntruder 'intruder.txt'
    $helperFull = (Resolve-Path -LiteralPath $HelperPath).Path
    $stdoutFile = Join-Path $caseIntruder 'apply.out'
    $stderrFile = Join-Path $caseIntruder 'apply.err'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $helperFull
    $psi.Arguments = @(
        'runtime-update-apply',
        '--data-dir', ('"{0}"' -f $dataIntruder),
        '--package-data', ('"{0}"' -f $packageIntruder),
        '--generated-settings', ('"{0}"' -f $genSettingsIntruder),
        '--generated-endpoint', ('"{0}"' -f $genEndpointIntruder),
        '--rollback-dir', ('"{0}"' -f $rbIntruder)
    ) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $suspended = $false
    $applyIntruderExit = -1
    try {
        $deadline = (Get-Date).AddSeconds(20)
        while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $rbIntruder) {
                [void][NativeProc]::NtSuspendProcess($proc.Handle)
                $suspended = $true
                Set-Content -LiteralPath $intruderFile -Value 'intruder' -Encoding ASCII
                break
            }
            Start-Sleep -Milliseconds 1
        }
        if ($suspended) {
            [void][NativeProc]::NtResumeProcess($proc.Handle)
            $suspended = $false
        }
        if (-not $proc.WaitForExit(30000)) {
            $proc.Kill()
            [void]$proc.WaitForExit(5000)
        }
        $applyIntruderExit = $proc.ExitCode
        $proc.StandardOutput.ReadToEnd() | Set-Content -LiteralPath $stdoutFile -Encoding ASCII
        $proc.StandardError.ReadToEnd() | Set-Content -LiteralPath $stderrFile -Encoding ASCII
    } finally {
        if ($suspended -and -not $proc.HasExited) {
            [void][NativeProc]::NtResumeProcess($proc.Handle)
        }
        if (-not $proc.HasExited) {
            $proc.Kill()
        }
        $proc.Dispose()
    }
    Assert-True ($applyIntruderExit -ne 0) 'intruder apply failed before mutation'
    Assert-True (Test-Path -LiteralPath $rbIntruder) 'intruder TX directory was preserved'
    Assert-True (Test-Path -LiteralPath $intruderFile) 'unexpected TX file was not deleted'
} finally {
    if (Test-Path -LiteralPath $script:tempRoot) {
        Remove-Item -LiteralPath $script:tempRoot -Recurse -Force -ErrorAction SilentlyContinue
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
