# SearchEngineConfig production filesystem roots contract tests.
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

$scriptsRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent $scriptsRoot
$templatePath = Join-Path $projectRoot (
    'deployment\SearchEngineServicePortable\source-data\Settings.json'
)

$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0
$script:tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SearchEngineProductionRoots-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $script:tempRoot | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding $false

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

function Write-Utf8File {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Invoke-Config {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $HelperPath @Arguments 2>&1 |
            ForEach-Object { $_.ToString() } | Out-String
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = $output
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Set-ConfigField {
    param(
        [string]$Json,
        [string]$Name,
        [string]$Replacement
    )
    $key = '"' + $Name + '"'
    $keyIndex = $Json.IndexOf($key)
    if ($keyIndex -lt 0) {
        throw "Did not find field $Name"
    }
    $colon = $Json.IndexOf([char]':', $keyIndex + $key.Length)
    if ($colon -lt 0) {
        throw "Did not find colon for $Name"
    }
    $i = $colon + 1
    while ($i -lt $Json.Length -and [char]::IsWhiteSpace($Json[$i])) {
        $i++
    }
    if ($i -lt $Json.Length -and $Json[$i] -eq '"') {
        $i++
        while ($i -lt $Json.Length) {
            if ($Json[$i] -eq '\' -and ($i + 1) -lt $Json.Length) {
                $i += 2
                continue
            }
            if ($Json[$i] -eq '"') {
                $i++
                break
            }
            $i++
        }
    } else {
        while ($i -lt $Json.Length -and $Json[$i] -ne ',' -and
            $Json[$i] -ne "`n" -and $Json[$i] -ne "`r")
        {
            $i++
        }
    }
    return $Json.Substring(0, $keyIndex) + $Replacement + $Json.Substring($i)
}

try {
    $templateJson = [IO.File]::ReadAllText($templatePath, $utf8NoBom)

    Write-Host '=== SearchEngine production roots tests ==='

    $inspect = Invoke-Config -Arguments @(
        'inspect', '--settings', $templatePath
    )
    Assert-True ($inspect.ExitCode -eq 0) 'inspect template exits 0'
    foreach ($key in @(
        'tlg_send_root=',
        'razn_output_dir=',
        'opis_base_dir=',
        'f12_base_dir='
    )) {
        Assert-True ($inspect.Output.Contains($key)) "inspect prints $key"
    }

    $positiveCases = @(
        @{ Name = 'C-root'; Field = 'tlg_send_root'; Value = 'C:\\' },
        @{ Name = 'D-F12'; Field = 'f12_base_dir'; Value = 'D:\\F12' },
        @{ Name = 'space'; Field = 'opis_base_dir'; Value = 'E:\\OPIS ADMIN' },
        @{ Name = 'unicode'; Field = 'opis_base_dir'; Value = "E:\\$([char]0x041E)$([char]0x041F)$([char]0x0418)$([char]0x0421)" }
    )
    foreach ($case in $positiveCases) {
        $json = Set-ConfigField -Json $templateJson -Name $case.Field `
            -Replacement ('"' + $case.Field + '": "' + $case.Value + '"')
        $path = Join-Path $script:tempRoot ($case.Name + '.json')
        Write-Utf8File -Path $path -Text $json
        $result = Invoke-Config -Arguments @('validate', '--settings', $path)
        Assert-True ($result.ExitCode -eq 0) (
            "positive $($case.Name) exits 0"
        )
        Assert-True ($result.Output.Contains('settings_valid=1')) (
            "positive $($case.Name) settings_valid=1"
        )
        Assert-True (-not $result.Output.Contains('directory is missing')) (
            "positive $($case.Name) does not require the directory to exist"
        )
    }

    $negativeCases = @(
        @{ Name = 'empty'; Replacement = '"tlg_send_root": ""'; Needle = 'must be a non-empty string' },
        @{ Name = 'relative'; Replacement = '"f12_base_dir": "F12"'; Needle = 'absolute local Windows path' },
        @{ Name = 'unc'; Replacement = '"opis_base_dir": "\\\\server\\share"'; Needle = 'absolute local Windows path' },
        @{ Name = 'number'; Replacement = '"tlg_send_root": 12'; Needle = 'must be a non-empty string' }
    )
    foreach ($case in $negativeCases) {
        $field = if ($case.Replacement -match '^"([^"]+)"') { $Matches[1] } else { '' }
        $json = Set-ConfigField -Json $templateJson -Name $field `
            -Replacement $case.Replacement
        $path = Join-Path $script:tempRoot ($case.Name + '.json')
        Write-Utf8File -Path $path -Text $json
        $result = Invoke-Config -Arguments @('validate', '--settings', $path)
        Assert-True ($result.ExitCode -ne 0) "negative $($case.Name) exits non-zero"
        Assert-True ($result.Output.Contains($case.Needle)) (
            "negative $($case.Name) reports $($case.Needle)"
        )
    }

    $oldJson = $templateJson
    foreach ($name in @(
        'tlg_send_root',
        'razn_output_dir',
        'opis_base_dir',
        'f12_base_dir'
    )) {
        $stripPattern = '[ \t]*"' + [regex]::Escape($name) + '"' +
            '[ \t]*:[ \t]*(?:"(?:[^"\\]|\\.)*"|\d+)[ \t]*,?\r?\n'
        $oldJson = [regex]::Replace($oldJson, $stripPattern, '')
    }
    $oldPath = Join-Path $script:tempRoot 'old-settings.json'
    Write-Utf8File -Path $oldPath -Text $oldJson
    $importedDefaults = Join-Path $script:tempRoot 'imported-defaults.json'
    $configureDefaults = Invoke-Config -Arguments @(
        'configure',
        '--template', $templatePath,
        '--output', $importedDefaults,
        '--import-settings', $oldPath,
        '--port', '15001',
        '--year', '2026',
        '--threads', '2',
        '--file-timeout', '120',
        '--prm-autodetect', '1',
        '--quiet'
    )
    Assert-True ($configureDefaults.ExitCode -eq 0) (
        'import without new fields exits 0'
    )
    $importedText = [IO.File]::ReadAllText($importedDefaults, $utf8NoBom)
    Assert-True ($importedText.Contains('"tlg_send_root": "D:\\"')) (
        'import without new fields keeps template tlg_send_root'
    )
    Assert-True ($importedText.Contains('"f12_base_dir": "D:\\F12"')) (
        'import without new fields keeps template f12_base_dir'
    )
    Assert-True ($importedText.Contains('"opis_base_dir": "D:\\OPIS_ADMIN"')) (
        'import without new fields keeps template opis_base_dir'
    )

    $customJson = Set-ConfigField -Json $templateJson -Name 'tlg_send_root' `
        -Replacement '"tlg_send_root": "E:\\tlg-imported"'
    $customJson = Set-ConfigField -Json $customJson -Name 'razn_output_dir' `
        -Replacement '"razn_output_dir": "E:\\razn-imported"'
    $customJson = Set-ConfigField -Json $customJson -Name 'opis_base_dir' `
        -Replacement '"opis_base_dir": "F:\\opis-imported"'
    $customJson = Set-ConfigField -Json $customJson -Name 'f12_base_dir' `
        -Replacement '"f12_base_dir": "G:\\f12-imported"'
    $customOld = Join-Path $script:tempRoot 'custom-old.json'
    Write-Utf8File -Path $customOld -Text $customJson
    $importedCustom = Join-Path $script:tempRoot 'imported-custom.json'
    $configureCustom = Invoke-Config -Arguments @(
        'configure',
        '--template', $templatePath,
        '--output', $importedCustom,
        '--import-settings', $customOld,
        '--port', '15001',
        '--year', '2026',
        '--threads', '2',
        '--file-timeout', '120',
        '--prm-autodetect', '1',
        '--quiet'
    )
    Assert-True ($configureCustom.ExitCode -eq 0) 'import custom roots exits 0'
    $customText = [IO.File]::ReadAllText($importedCustom, $utf8NoBom)
    Assert-True ($customText.Contains('"tlg_send_root": "E:\\tlg-imported"')) (
        'import keeps operator tlg_send_root'
    )
    Assert-True ($customText.Contains('"razn_output_dir": "E:\\razn-imported"')) (
        'import keeps operator razn_output_dir'
    )
    Assert-True ($customText.Contains('"opis_base_dir": "F:\\opis-imported"')) (
        'import keeps operator opis_base_dir'
    )
    Assert-True ($customText.Contains('"f12_base_dir": "G:\\f12-imported"')) (
        'import keeps operator f12_base_dir'
    )
}
finally {
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
