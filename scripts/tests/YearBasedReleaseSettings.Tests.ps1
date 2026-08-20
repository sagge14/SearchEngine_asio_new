# Year-based fresh release Settings generation tests.
# Uses only temporary files and never runs a build, package, installer, or service.
[CmdletBinding()]
param(
    [string]$HelperPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptsRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent $scriptsRoot
$generatorPath = Join-Path $scriptsRoot 'Prepare-YearBasedReleaseSettings.ps1'
$packagerPath = Join-Path $scriptsRoot 'New-SearchEngineServicePackage.ps1'
$templatePath = Join-Path $projectRoot (
    'deployment\SearchEngineServicePortable\source-data\Settings.json'
)
$script:failures = New-Object System.Collections.Generic.List[string]
$script:passed = 0
$script:tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'YearBasedReleaseSettings-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $script:tempRoot | Out-Null
$utf8NoBom = New-Object Text.UTF8Encoding($false)

function Assert-True([bool]$Condition, [string]$Message) {
    if ($Condition) {
        $script:passed++
        Write-Host "PASS: $Message"
    } else {
        $script:failures.Add($Message) | Out-Null
        Write-Host "FAIL: $Message"
    }
}

function Assert-Equal($Expected, $Actual, [string]$Message) {
    if ($Expected -ceq $Actual) {
        Assert-True $true $Message
    } else {
        Assert-True $false (
            "$Message (expected='$Expected', actual='$Actual')"
        )
    }
}

function Assert-Throws([scriptblock]$Action, [string]$ExpectedText, [string]$Message) {
    try {
        & $Action | Out-Null
        Assert-True $false "$Message (no error was raised)"
    } catch {
        Assert-True ($_.Exception.Message -like "*$ExpectedText*") (
            "$Message (error='$($_.Exception.Message)')"
        )
    }
}

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 |
        ConvertFrom-Json
}

function Write-Json([string]$Path, $Value) {
    $text = $Value | ConvertTo-Json -Depth 100
    $text = ($text -replace "`r?`n", "`r`n").TrimEnd("`r", "`n") + "`r`n"
    [IO.File]::WriteAllText($Path, $text, $utf8NoBom)
}

function New-MutatedTemplate(
    [string]$Name,
    [scriptblock]$Mutation
) {
    $value = Read-Json $templatePath
    & $Mutation $value
    $path = Join-Path $script:tempRoot $Name
    Write-Json $path $value
    return $path
}

function Invoke-Prepare {
    param(
        [string]$Template,
        [string]$Output,
        [Nullable[int]]$Year,
        [string]$AllowedRoot = $script:tempRoot,
        [string]$Validator
    )
    $arguments = @{
        TemplatePath = $Template
        OutputPath = $Output
        AllowedOutputRoot = $AllowedRoot
    }
    if ($PSBoundParameters.ContainsKey('Year')) {
        $arguments.Year = [int]$Year
    }
    if (-not [string]::IsNullOrWhiteSpace($Validator)) {
        $arguments.ConfigToolPath = $Validator
    }
    return & $generatorPath @arguments
}

try {
    Write-Host '=== Year-based release Settings tests ==='
    $templateHashBefore = (Get-FileHash -LiteralPath $templatePath -Algorithm SHA256).Hash
    $template = Read-Json $templatePath
    $basePort = [int]$template.config.asio_port

    foreach ($case in @(
        @{ Year = 2026; Digit = 6 },
        @{ Year = 2027; Digit = 7 },
        @{ Year = 2030; Digit = 0 }
    )) {
        $output = Join-Path $script:tempRoot ("settings-$($case.Year).json")
        $result = Invoke-Prepare -Template $templatePath -Output $output `
            -Year $case.Year
        $generated = Read-Json $output
        $expectedPort = [Math]::Floor($basePort / 10) * 10 + $case.Digit
        Assert-Equal ([string]$case.Year) ([string]$generated.config.year) (
            "explicit -Year $($case.Year) is written to config.year"
        )
        Assert-Equal ([int]$expectedPort) ([int]$generated.config.asio_port) (
            "year $($case.Year) produces port digit $($case.Digit)"
        )
        Assert-Equal ([int][Math]::Floor($basePort / 10)) (
            [int][Math]::Floor(([int]$generated.config.asio_port) / 10)
        ) "year $($case.Year) preserves all other port digits"
        Assert-Equal ([int]$case.Year) ([int]$result.Year) (
            'generator returns the explicitly selected year'
        )
    }

    $defaultYear = (Get-Date).Year
    $defaultOutput = Join-Path $script:tempRoot 'settings-default-year.json'
    $defaultResult = Invoke-Prepare -Template $templatePath -Output $defaultOutput
    $defaultGenerated = Read-Json $defaultOutput
    Assert-Equal $defaultYear ([int]$defaultResult.Year) (
        'omitted -Year selects the current calendar year once'
    )
    Assert-Equal ([string]$defaultYear) ([string]$defaultGenerated.config.year) (
        'default calendar year is written to config.year'
    )

    $stableOne = Join-Path $script:tempRoot 'stable-one.json'
    $stableTwo = Join-Path $script:tempRoot 'stable-two.json'
    Invoke-Prepare -Template $templatePath -Output $stableOne -Year 2027 | Out-Null
    Invoke-Prepare -Template $templatePath -Output $stableTwo -Year 2027 | Out-Null
    Assert-Equal (
        (Get-FileHash -LiteralPath $stableOne -Algorithm SHA256).Hash
    ) (
        (Get-FileHash -LiteralPath $stableTwo -Algorithm SHA256).Hash
    ) 'same template and -Year produce byte-for-byte stable output'

    $expected = Read-Json $templatePath
    $expected.config.year = '2027'
    $expected.config.asio_port = [int]([Math]::Floor($basePort / 10) * 10 + 7)
    $actual = Read-Json $stableOne
    Assert-Equal (
        ($expected | ConvertTo-Json -Depth 100 -Compress)
    ) (
        ($actual | ConvertTo-Json -Depth 100 -Compress)
    ) 'all non-target Settings values remain unchanged'

    foreach ($field in @(
        'query_word_match',
        'excluded_subtrees',
        'indexed_extensions',
        'include_extensionless_files'
    )) {
        Assert-Equal (
            ($template.config.$field | ConvertTo-Json -Depth 20 -Compress)
        ) (
            ($actual.config.$field | ConvertTo-Json -Depth 20 -Compress)
        ) "canonical BLOCK 2B field '$field' remains unchanged"
    }

    Assert-Equal $templateHashBefore (
        (Get-FileHash -LiteralPath $templatePath -Algorithm SHA256).Hash
    ) 'tracked fresh template is not modified by generation'

    $invalidYearOutput = Join-Path $script:tempRoot 'invalid-year.json'
    Assert-Throws {
        Invoke-Prepare -Template $templatePath -Output $invalidYearOutput -Year 1999
    } 'Year must be inside 2000..2099' 'invalid year has a clear error'

    $hostPath = (Get-Process -Id $PID).Path
    $previousErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $invalidYearProcessOutput = & $hostPath -NoProfile -ExecutionPolicy Bypass `
            -File $generatorPath -TemplatePath $templatePath `
            -OutputPath $invalidYearOutput -Year 1999 2>&1
        $invalidYearExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorPreference
    }
    Assert-True ($invalidYearExitCode -ne 0) 'invalid year returns a non-zero process exit code'
    Assert-True (
        (($invalidYearProcessOutput | Out-String) -like '*Year must be inside 2000..2099*')
    ) 'invalid year process output contains the range error'

    foreach ($invalidPort in @(
        @{ Name = 'wrong-type'; Value = '15006'; Error = 'must be an integer' },
        @{ Name = 'negative'; Value = -1; Error = 'inside 1..65535' },
        @{ Name = 'zero'; Value = 0; Error = 'inside 1..65535' },
        @{ Name = 'too-high'; Value = 65536; Error = 'inside 1..65535' },
        @{ Name = 'generated-too-high'; Value = 65535; Error = 'Generated release port' }
    )) {
        $badTemplate = New-MutatedTemplate "port-$($invalidPort.Name).json" {
            param($json)
            $json.config.asio_port = $invalidPort.Value
        }
        Assert-Throws {
            Invoke-Prepare -Template $badTemplate `
                -Output (Join-Path $script:tempRoot "out-$($invalidPort.Name).json") `
                -Year 2026
        } $invalidPort.Error "invalid port '$($invalidPort.Name)' is rejected"
    }

    foreach ($missingField in @('year', 'asio_port')) {
        $badTemplate = New-MutatedTemplate "missing-$missingField.json" {
            param($json)
            $json.config.PSObject.Properties.Remove($missingField)
        }
        Assert-Throws {
            Invoke-Prepare -Template $badTemplate `
                -Output (Join-Path $script:tempRoot "out-missing-$missingField.json") `
                -Year 2026
        } "missing canonical config.$missingField" (
            "missing canonical field '$missingField' is rejected without schema creation"
        )
    }

    $wrongYearTypeTemplate = New-MutatedTemplate 'year-wrong-type.json' {
        param($json)
        $json.config.year = 2026
    }
    Assert-Throws {
        Invoke-Prepare -Template $wrongYearTypeTemplate `
            -Output (Join-Path $script:tempRoot 'out-year-wrong-type.json') `
            -Year 2026
    } 'config.year must be a four-digit string' (
        'wrong-type canonical config.year is rejected'
    )

    $wrongConfigTypeTemplate = New-MutatedTemplate 'config-wrong-type.json' {
        param($json)
        $json.config = 'not-an-object'
    }
    Assert-Throws {
        Invoke-Prepare -Template $wrongConfigTypeTemplate `
            -Output (Join-Path $script:tempRoot 'out-config-wrong-type.json') `
            -Year 2026
    } 'canonical config object' 'wrong-type config object is rejected'

    $outsideRoot = Join-Path (Split-Path -Parent $script:tempRoot) (
        'outside-' + [Guid]::NewGuid().ToString('N') + '.json'
    )
    Assert-Throws {
        Invoke-Prepare -Template $templatePath -Output $outsideRoot -Year 2026
    } 'must stay inside AllowedOutputRoot' 'output outside staging root is rejected'

    Assert-Throws {
        & $generatorPath -TemplatePath $templatePath `
            -OutputPath (Join-Path $script:tempRoot 'missing\settings.json') `
            -Year 2026
    } 'output directory was not found' 'unwritable/missing output directory is rejected'

    $rejectValidator = Join-Path $script:tempRoot 'reject-validator.cmd'
    [IO.File]::WriteAllText($rejectValidator, "@exit /b 7`r`n", [Text.Encoding]::ASCII)
    $preservedOutput = Join-Path $script:tempRoot 'preserved.json'
    $preservedBytes = [Text.Encoding]::UTF8.GetBytes('previous-valid-result')
    [IO.File]::WriteAllBytes($preservedOutput, $preservedBytes)
    $preservedHash = (Get-FileHash -LiteralPath $preservedOutput -Algorithm SHA256).Hash
    Assert-Throws {
        Invoke-Prepare -Template $templatePath -Output $preservedOutput `
            -Year 2026 -Validator $rejectValidator
    } 'Canonical Settings validation failed' 'validation failure is reported'
    Assert-Equal $preservedHash (
        (Get-FileHash -LiteralPath $preservedOutput -Algorithm SHA256).Hash
    ) 'validation failure does not damage an existing staging Settings file'

    if (-not [string]::IsNullOrWhiteSpace($HelperPath)) {
        $canonicalOutput = Join-Path $script:tempRoot 'canonical-validated.json'
        Invoke-Prepare -Template $templatePath -Output $canonicalOutput `
            -Year 2026 -Validator $HelperPath | Out-Null
        Assert-True (Test-Path -LiteralPath $canonicalOutput -PathType Leaf) (
            'generated Settings passes SearchEngineConfig canonical validation'
        )
    }

    $packagerText = Get-Content -LiteralPath $packagerPath -Raw
    $preparePosition = $packagerText.IndexOf('Prepare-YearBasedReleaseSettings.ps1')
    $validationPosition = $packagerText.IndexOf(
        '& $configToolPath validate --settings $generatedSettingsPath'
    )
    Assert-True ($preparePosition -ge 0 -and $validationPosition -gt $preparePosition) (
        'canonical packager generates staging Settings before package validation'
    )
    Assert-True ($packagerText -like '*AllowedOutputRoot = $stagingDirectory*') (
        'canonical packager restricts generated output to its staging directory'
    )
    Assert-True ($packagerText -notlike '*Copy-Item -LiteralPath $SettingsPath*') (
        'canonical packager does not copy the unchanged source template'
    )
}
finally {
    if (Test-Path -LiteralPath $script:tempRoot) {
        Remove-Item -LiteralPath $script:tempRoot -Recurse -Force `
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
