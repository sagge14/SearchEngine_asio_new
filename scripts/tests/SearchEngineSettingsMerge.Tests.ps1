# SearchEngineConfig template + import-settings merge contract tests.
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
$sourceTemplatePath = Join-Path $projectRoot (
    'deployment\SearchEngineServicePortable\source-data\Settings.json'
)

$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0
$script:tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'SearchEngineSettingsMerge-' + [Guid]::NewGuid().ToString('N')
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

function Get-ConfigureArguments {
    param(
        [string]$TemplatePath,
        [string]$OutputPath,
        [string]$ImportPath,
        [int]$Port = 15001,
        [int]$Year = 2026
    )
    return @(
        'configure',
        '--template', $TemplatePath,
        '--output', $OutputPath,
        '--import-settings', $ImportPath,
        '--port', ([string]$Port),
        '--year', ([string]$Year),
        '--threads', '2',
        '--file-timeout', '120',
        '--prm-autodetect', '1',
        '--quiet'
    )
}

try {
    Write-Host '=== SearchEngine Settings merge contract tests ==='

    $baseTemplate = Get-Content -LiteralPath $sourceTemplatePath -Raw -Encoding UTF8 |
        ConvertFrom-Json

    $mergeTemplate = $baseTemplate | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $mergeTemplate.config | Add-Member -NotePropertyName 'merge_probe' -NotePropertyValue ([ordered]@{
        existing = 'template-default'
        new_field = 42
        nested = [ordered]@{
            a = 'template-a'
            c = 3
        }
    }) -Force
    $mergeTemplate | Add-Member -NotePropertyName 'template_top_probe' `
        -NotePropertyValue 'template-only' -Force

    $mergeOld = $baseTemplate | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $mergeOld.config | Add-Member -NotePropertyName 'merge_probe' -NotePropertyValue ([ordered]@{
        existing = 'user-value'
        custom_old = 'keep-me'
        nested = [ordered]@{
            a = 'user-a'
            b = 2
        }
    }) -Force
    $mergeOld | Add-Member -NotePropertyName 'installed_top_probe' `
        -NotePropertyValue 'keep-installed-top-level' -Force

    $templatePath = Join-Path $script:tempRoot 'merge-template.json'
    $oldPath = Join-Path $script:tempRoot 'merge-old.json'
    $outputPath = Join-Path $script:tempRoot 'merge-output.json'
    Write-Utf8File -Path $templatePath -Text ($mergeTemplate | ConvertTo-Json -Depth 32)
    Write-Utf8File -Path $oldPath -Text ($mergeOld | ConvertTo-Json -Depth 32)

    $configure = Invoke-Config -Arguments (Get-ConfigureArguments `
        -TemplatePath $templatePath -OutputPath $outputPath -ImportPath $oldPath)
    Assert-True ($configure.ExitCode -eq 0) 'configure template+import exits 0'

    $merged = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $probe = $merged.config.merge_probe

    Assert-True ($probe.existing -eq 'user-value') (
        'existing installed value preserved over template default'
    )
    Assert-True ($probe.new_field -eq 42) (
        'new template-only field receives template default'
    )
    Assert-True ($probe.custom_old -eq 'keep-me') (
        'unknown old field preserved when absent from template'
    )
    Assert-True ($probe.nested.a -eq 'user-a') 'nested merge keeps old leaf value'
    Assert-True ($probe.nested.b -eq 2) 'nested merge keeps old-only leaf'
    Assert-True ($probe.nested.c -eq 3) 'nested merge keeps template-only leaf'
    Assert-True ($merged.template_top_probe -eq 'template-only') (
        'unknown template top-level field is preserved'
    )
    Assert-True ($merged.installed_top_probe -eq 'keep-installed-top-level') (
        'unknown installed top-level field is preserved'
    )

    # A generated package template may contain a newer release year/port, but
    # configure-interactive passes the inspected installed values as explicit
    # choices unless the user changes them. Those user values remain dominant.
    $releaseTemplate = $baseTemplate | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $releaseTemplate.config.year = '2027'
    $releaseTemplate.config.asio_port = 15007
    $installed = $baseTemplate | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $installed.config.year = '2024'
    $installed.config.asio_port = 1544
    $installed.config | Add-Member -NotePropertyName 'installed_unknown' `
        -NotePropertyValue 'keep-config-value' -Force
    $installed | Add-Member -NotePropertyName 'installed_unknown_top' `
        -NotePropertyValue 'keep-top-value' -Force

    $releaseTemplatePath = Join-Path $script:tempRoot 'release-template.json'
    $installedPath = Join-Path $script:tempRoot 'installed.json'
    $installedOutputPath = Join-Path $script:tempRoot 'installed-output.json'
    Write-Utf8File -Path $releaseTemplatePath `
        -Text ($releaseTemplate | ConvertTo-Json -Depth 32)
    Write-Utf8File -Path $installedPath `
        -Text ($installed | ConvertTo-Json -Depth 32)

    $installedConfigure = Invoke-Config -Arguments (Get-ConfigureArguments `
        -TemplatePath $releaseTemplatePath -OutputPath $installedOutputPath `
        -ImportPath $installedPath -Port 1544 -Year 2024)
    Assert-True ($installedConfigure.ExitCode -eq 0) (
        'configure with generated release template and installed Settings exits 0'
    )
    $installedMerged = Get-Content -LiteralPath $installedOutputPath `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-True ($installedMerged.config.year -eq '2024') (
        'installed/user year is not replaced by package release year'
    )
    Assert-True ($installedMerged.config.asio_port -eq 1544) (
        'installed/user port is not replaced by package release port'
    )
    Assert-True ($installedMerged.config.installed_unknown -eq 'keep-config-value') (
        'unknown installed config field survives configure/import'
    )
    Assert-True ($installedMerged.installed_unknown_top -eq 'keep-top-value') (
        'unknown installed top-level field survives configure/import'
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
