# Strict ASCII helpers for portable cmd.exe console scripts (Windows 7+).
# Dot-source from packaging scripts and regression tests.

function Assert-StrictAsciiText {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory)][string]$Path
    )

    for ($i = 0; $i -lt $Text.Length; $i++) {
        $code = [int][char]$Text[$i]
        if ($code -gt 127) {
            throw (
                "Non-ASCII character U+{0:X4} at index {1} in console script " +
                "(strict ASCII required for cmd.exe / Windows 7 portable packages): {2}"
            ) -f $code, $i, $Path
        }
    }
}

function Get-StrictAsciiEncoding {
    return [Text.Encoding]::GetEncoding(
        'us-ascii',
        [Text.EncoderExceptionFallback]::new(),
        [Text.DecoderExceptionFallback]::new()
    )
}

function Write-StrictAsciiText {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text
    )

    Assert-StrictAsciiText -Text $Text -Path $Path
    [IO.File]::WriteAllText($Path, $Text, (Get-StrictAsciiEncoding))
}

function Test-StrictAsciiBytes {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path
    )

    $bytes = [IO.File]::ReadAllBytes($Path)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -gt 127) {
            return $false
        }
    }
    return $true
}
