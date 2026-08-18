# SearchEngineConfig validate-prefix-map contract tests.
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
    'SearchEnginePrefixMap-' + [Guid]::NewGuid().ToString('N')
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
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Invoke-ValidatePrefixMap {
    param([string]$Path)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $HelperPath validate-prefix-map --path $Path 2>&1 |
            ForEach-Object { $_.ToString() } | Out-String
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = $output
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Assert-ValidPrefixMap {
    param([string]$Path, [string]$Name)
    $result = Invoke-ValidatePrefixMap -Path $Path
    Assert-True ($result.ExitCode -eq 0) "$Name exits 0"
    Assert-True ($result.Output -match 'prefix_map_valid=1') (
        "$Name reports prefix_map_valid=1"
    )
}

function Assert-InvalidPrefixMap {
    param(
        [string]$Path,
        [string]$Name,
        [string]$ExpectedError
    )
    $result = Invoke-ValidatePrefixMap -Path $Path
    Assert-True ($result.ExitCode -ne 0) "$Name exits non-zero"
    Assert-True ($result.Output -match 'prefix_map_valid=0') (
        "$Name reports prefix_map_valid=0"
    )
    Assert-True ($result.Output.Contains($ExpectedError)) (
        "$Name reports: $ExpectedError"
    )
}

Write-Host '=== SearchEngine prefix_map validation tests ==='

try {
    $validPath = Join-Path $script:tempRoot 'valid.json'
    Write-Utf8File -Path $validPath -Text '{"prefix":"D:\\inbox\\","map":{"op":"buffer"}}'
    Assert-ValidPrefixMap -Path $validPath -Name 'valid prefix + map'

    $emptyMapPath = Join-Path $script:tempRoot 'empty-map.json'
    Write-Utf8File -Path $emptyMapPath -Text '{"prefix":"","map":{}}'
    Assert-ValidPrefixMap -Path $emptyMapPath -Name 'empty map'

    $cyrData = -join @(0x0434, 0x0430, 0x043D, 0x043D, 0x044B, 0x0435 |
        ForEach-Object { [char]$_ })
    $cyrOp = -join @(0x043E, 0x043F, 0x0435, 0x0440, 0x0430, 0x0442, 0x043E, 0x0440 |
        ForEach-Object { [char]$_ })
    $cyrDir = -join @(0x0432, 0x043B, 0x043E, 0x0436, 0x0435, 0x043D, 0x0438, 0x044F |
        ForEach-Object { [char]$_ })
    $unicodePath = Join-Path $script:tempRoot 'unicode.json'
    Write-Utf8File -Path $unicodePath -Text (
        '{"prefix":"C:\\' + $cyrData + '\\","map":{"' + $cyrOp + '":"' + $cyrDir + '"}}'
    )
    Assert-ValidPrefixMap -Path $unicodePath -Name 'Unicode paths/operators'

    $missingPath = Join-Path $script:tempRoot 'missing.json'
    Assert-InvalidPrefixMap -Path $missingPath -Name 'missing file' `
        -ExpectedError 'prefix map file does not exist'

    $invalidJsonPath = Join-Path $script:tempRoot 'invalid.json'
    Write-Utf8File -Path $invalidJsonPath -Text '{not-json'
    Assert-InvalidPrefixMap -Path $invalidJsonPath -Name 'invalid JSON' `
        -ExpectedError 'prefix map JSON is invalid'

    $rootArrayPath = Join-Path $script:tempRoot 'root-array.json'
    Write-Utf8File -Path $rootArrayPath -Text '[]'
    Assert-InvalidPrefixMap -Path $rootArrayPath -Name 'root array' `
        -ExpectedError 'prefix map root must be an object'

    $missingPrefixPath = Join-Path $script:tempRoot 'missing-prefix.json'
    Write-Utf8File -Path $missingPrefixPath -Text '{"map":{}}'
    Assert-InvalidPrefixMap -Path $missingPrefixPath -Name 'missing prefix' `
        -ExpectedError 'prefix map is missing prefix'

    $prefixTypePath = Join-Path $script:tempRoot 'prefix-type.json'
    Write-Utf8File -Path $prefixTypePath -Text '{"prefix":1,"map":{}}'
    Assert-InvalidPrefixMap -Path $prefixTypePath -Name 'prefix wrong type' `
        -ExpectedError 'prefix map prefix must be a string'

    $missingMapPath = Join-Path $script:tempRoot 'missing-map.json'
    Write-Utf8File -Path $missingMapPath -Text '{"prefix":""}'
    Assert-InvalidPrefixMap -Path $missingMapPath -Name 'missing map' `
        -ExpectedError 'prefix map is missing map'

    $mapTypePath = Join-Path $script:tempRoot 'map-type.json'
    Write-Utf8File -Path $mapTypePath -Text '{"prefix":"","map":[]}'
    Assert-InvalidPrefixMap -Path $mapTypePath -Name 'map wrong type' `
        -ExpectedError 'prefix map map must be an object'

    $mapValuePath = Join-Path $script:tempRoot 'map-value.json'
    Write-Utf8File -Path $mapValuePath -Text '{"prefix":"","map":{"op":1}}'
    Assert-InvalidPrefixMap -Path $mapValuePath -Name 'map value not string' `
        -ExpectedError 'prefix map map values must be strings'
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
