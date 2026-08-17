function Assert-SearchEngineConfigSourceFreshness {
    param(
        [Parameter(Mandatory)][string]$ConfigToolPath,
        [Parameter(Mandatory)][string]$ProjectRoot
    )

    $configSourcePath = Join-Path $ProjectRoot 'tools\config\main.cpp'
    if (-not (Test-Path -LiteralPath $configSourcePath -PathType Leaf)) {
        throw "SearchEngineConfig source was not found: $configSourcePath"
    }

    $exeTime = (Get-Item -LiteralPath $ConfigToolPath).LastWriteTimeUtc
    $sourceTime = (Get-Item -LiteralPath $configSourcePath).LastWriteTimeUtc
    if ($exeTime -lt $sourceTime) {
        throw (
            'SearchEngineConfig.exe is older than tools/config/main.cpp. ' +
            'Rebuild the Release target before packaging.'
        )
    }
}

function Assert-SearchEngineConfigAutoPadContract {
    param(
        [Parameter(Mandatory)][string]$ConfigToolPath,
        [Parameter(Mandatory)][string]$TemplatePath
    )

    $settingsTemplate = Get-Content -LiteralPath $TemplatePath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if (-not $settingsTemplate.config) {
        throw "Settings template is missing config: $TemplatePath"
    }

    $cases = @(
        @{ Name = 'both_empty'; Prm = ''; Prd = '' },
        @{ Name = 'prm_empty'; Prm = ''; Prd = 'D:\BASES_PRD' },
        @{ Name = 'prd_empty'; Prm = 'D:\BASES'; Prd = '' },
        @{ Name = 'both_set'; Prm = 'D:\BASES'; Prd = 'D:\BASES_PRD' }
    )

    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'searchengine-config-autopad-' + [Guid]::NewGuid().ToString('N')
    )
    New-Item -ItemType Directory -Path $tempRoot | Out-Null

    try {
        foreach ($case in $cases) {
            $settings = [pscustomobject]@{
                Files = @($settingsTemplate.Files)
                config = [pscustomobject]@{}
            }
            foreach ($property in $settingsTemplate.config.PSObject.Properties) {
                $settings.config |
                    Add-Member -NotePropertyName $property.Name `
                        -NotePropertyValue $property.Value
            }
            $settings.config.prm_base_dir = $case.Prm
            $settings.config.prd_base_dir = $case.Prd

            $settingsPath = Join-Path $tempRoot ($case.Name + '.json')
            $settingsJson = $settings | ConvertTo-Json -Depth 20
            [IO.File]::WriteAllText(
                $settingsPath,
                $settingsJson,
                (New-Object Text.UTF8Encoding($false))
            )

            $output = & $ConfigToolPath validate --settings $settingsPath 2>&1 |
                Out-String
            if ($LASTEXITCODE -ne 0) {
                throw (
                    "SearchEngineConfig rejected empty AutoPad contract case " +
                    "'$($case.Name)': exit=$LASTEXITCODE output=$output"
                )
            }
            if ($output -match 'must be a non-empty string') {
                throw (
                    "SearchEngineConfig still requires non-empty AutoPad paths " +
                    "for case '$($case.Name)': $output"
                )
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $tempRoot) {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force
        }
    }
}
