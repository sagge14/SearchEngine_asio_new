# Regenerate .zag test files: From=<key> bytes copied exactly from EXPORT.INI (ANSI/CP1251).
# No encoding conversion — key in .zag must match the line above "s=..." in EXPORT.INI byte-for-byte.

$dictPath = 'D:\BASES_PRD\EXPORT.INI'
if (-not (Test-Path $dictPath)) {
    Write-Error "EXPORT.INI not found: $dictPath"
    exit 1
}

$enc1251 = [System.Text.Encoding]::GetEncoding(1251)

$lines = [System.IO.File]::ReadAllLines($dictPath, $enc1251)
$keys = [System.Collections.Generic.List[string]]::new()

for ($i = 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^s=') {
        $key = $lines[$i - 1].Trim()
        if ($key -and $key -notmatch '^s=' -and $key -notmatch '^<') {
            $keys.Add($key) | Out-Null
            if ($keys.Count -ge 5) { break }
        }
    }
}

if ($keys.Count -lt 5) {
    Write-Error "Expected at least 5 dict keys, got $($keys.Count)"
    exit 1
}

function Write-ZagFile {
    param([string]$Path, [string]$FromKey)
    $content = "Header=1`r`nFrom=$FromKey`r`nOther=2`r`n"
    [System.IO.File]::WriteAllText($Path, $content, $enc1251)
}

$targets = @(
    (Join-Path $PSScriptRoot 'testdata_real_ansi')
    'D:\in'
)

for ($i = 0; $i -lt 5; $i++) {
    $name = "t$($i + 1).zag"
    foreach ($dir in $targets) {
        if (Test-Path $dir) {
            Write-ZagFile (Join-Path $dir $name) $keys[$i]
        }
    }
}

Get-ChildItem 'D:\in\*.bak' -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "Done: t1..t5.zag with ANSI keys from EXPORT.INI:"
for ($i = 0; $i -lt 5; $i++) {
    Write-Host "  t$($i+1): From=$($keys[$i])"
}
