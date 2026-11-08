# Tests the exact PowerShell command embedded in Verify-Package.bat.
# Legacy mode runs the real PowerShell 2.0 / CLR 2.0, with no emulation or compilation.
[CmdletBinding()]
param(
    [switch]$IncludeLegacyPowerShell,
    [string[]]$PackageDirectories = @(),
    [string]$ReportPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$template = Join-Path $project 'deployment\SearchEngineServicePortable\Verify-Package-Windows7.bat'
$engine = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempParent ('SE-VerifyPackage-' + [Guid]::NewGuid().ToString('N'))
$results = New-Object Collections.Generic.List[object]
$versions = @('5.1')
if ($IncludeLegacyPowerShell) { $versions += '2.0' }
function Read-Verifier([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    $match = [regex]::Match($text, '(?m)^.* -Command "(.*)"\r?$')
    if (-not $match.Success) { throw ('Cannot extract verifier command: ' + $Path) }
    return $match.Groups[1].Value
}
function Invoke-Verifier([string]$Body, [string]$Directory, [string]$Version, [string]$Name, [string]$ExpectedError) {
    $prefix = '$ErrorActionPreference=''Stop''; if ($PSVersionTable.PSVersion.Major -ne ' + $Version.Split('.')[0] + ') { throw ''Wrong PowerShell runtime'' }; '
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($prefix + $Body))
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $engine
    $runtimeArgs = if ($Version -eq '2.0') { '-Version 2 ' } else { '' }
    $info.Arguments = $runtimeArgs + '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' + $encoded
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['PACKAGE_ROOT'] = $Directory + '\'
    $info.EnvironmentVariables['CHECKSUM_FILE'] = Join-Path $Directory 'package-checksums.sha256'
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'Verifier fixture timeout' }
        $output = $stdout.Result + $stderr.Result
        $expectedExit = if ($ExpectedError) { 1 } else { 0 }
        $passed = $process.ExitCode -eq $expectedExit -and (-not $ExpectedError -or $output.Contains($ExpectedError))
        $results.Add([pscustomobject]@{ Name=$Name; PowerShell=$Version; ExpectedExit=$expectedExit;
            ExitCode=$process.ExitCode; Passed=$passed; Output=$output })
        Write-Host (('{0}: PowerShell {1}, {2}' -f $(if ($passed) { 'PASS' } else { 'FAIL' }), $Version, $Name))
    } finally { $process.Dispose() }
}
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $body = Read-Verifier $template
    $unicodeName = -join [char[]](1056,1091,1082,1086,1074,1086,1076,1089,1090,1074,1086)
    foreach ($mode in @('valid','damaged','missing','empty','invalid-record','absolute-path','traversal','mutable-data','token','request')) {
        $directory = Join-Path $root ($mode + ' & ! (2026) ' + $unicodeName)
        New-Item -ItemType Directory -Path (Join-Path $directory 'tutorials') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $directory 'data') | Out-Null
        $leaf = $unicodeName + '.html'
        $payload = Join-Path $directory ('tutorials\' + $leaf)
        [IO.File]::WriteAllText($payload,'known payload',[Text.Encoding]::UTF8)
        $hash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines = @($hash + '  tutorials\' + $leaf)
        $expectedError = ''
        switch ($mode) {
            'damaged' { [IO.File]::WriteAllText($payload,'changed payload'); $expectedError = 'Package file is damaged:' }
            'missing' { Remove-Item -LiteralPath $payload; $expectedError = 'Package file is missing:' }
            'empty' { $lines = @(); $expectedError = 'No protected package files were listed.' }
            'invalid-record' { $lines = @('invalid'); $expectedError = 'Invalid checksum record.' }
            'absolute-path' { $lines = @($hash + '  ' + $payload); $expectedError = 'Unsafe checksum path.' }
            'traversal' { $lines = @($hash + '  ..\outside.txt'); $expectedError = 'Checksum path escapes the package.' }
            'mutable-data' { $lines += ($hash + '  data\Settings.json'); [IO.File]::WriteAllText((Join-Path $directory 'data\Settings.json'),'editable data') }
            'token' { [IO.File]::WriteAllText((Join-Path $directory 'data\searchclient-auth-token.json'),'test'); $expectedError = 'auth token must be stored' }
            'request' { [IO.File]::WriteAllText((Join-Path $directory 'data\searchclient-auth-request.json'),'test'); $expectedError = 'unsigned auth request must not be stored' }
        }
        [IO.File]::WriteAllLines((Join-Path $directory 'package-checksums.sha256'),[string[]]$lines,(New-Object Text.UTF8Encoding($false)))
        $before = @(Get-ChildItem -LiteralPath $directory -Recurse -File | ForEach-Object {
            [pscustomobject]@{ Path=$_.FullName; Hash=(Get-FileHash -LiteralPath $_.FullName).Hash }
        })
        foreach ($version in $versions) { Invoke-Verifier $body $directory $version $mode $expectedError }
        foreach ($file in $before) {
            if (-not [IO.File]::Exists($file.Path) -or (Get-FileHash -LiteralPath $file.Path).Hash -ne $file.Hash) {
                throw ('Verifier changed a package file: ' + $file.Path)
            }
        }
    }
    foreach ($directory in $PackageDirectories) {
        $packagedBody = Read-Verifier (Join-Path $directory 'Verify-Package.bat')
        foreach ($version in $versions) { Invoke-Verifier $packagedBody $directory $version (Split-Path -Leaf $directory) '' }
    }
    if ($ReportPath) { $results.ToArray() | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
    $failed = @($results | Where-Object { -not $_.Passed })
    if ($failed.Count -gt 0) { throw ($failed | ConvertTo-Json -Depth 6) }
    Write-Host ('Package verifier tests passed: ' + $results.Count + '; runtimes=' + ($versions -join ', '))
} finally {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($root)) -ne $tempParent -or
            -not [IO.Path]::GetFileName($root).StartsWith('SE-VerifyPackage-')) { throw 'Unsafe fixture cleanup path' }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
