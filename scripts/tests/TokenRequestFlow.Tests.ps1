[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$IssuerExe,
    [Parameter(Mandatory = $true)][string]$AuthDbExe,
    [Parameter(Mandatory = $true)][string]$OpenSslExe,
    [Parameter(Mandatory = $true)][string]$WorkRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$IssuerExe = (Resolve-Path -LiteralPath $IssuerExe).Path
$AuthDbExe = (Resolve-Path -LiteralPath $AuthDbExe).Path
$OpenSslExe = (Resolve-Path -LiteralPath $OpenSslExe).Path
$workBase = [IO.Path]::GetFullPath($WorkRoot)
$runRoot = Join-Path $workBase ('request-flow-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)
$password = [guid]::NewGuid().ToString('N')
$utf8 = New-Object Text.UTF8Encoding($false)

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "PASS: $Message"
}

function File-Hash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)) }
    finally { $stream.Dispose(); $hash.Dispose() }
}

function Run-Tool([string]$Exe, [string[]]$ToolArgs, [string]$InputText = '') {
    if ($Exe -eq $IssuerExe -and $ToolArgs -notcontains '--language') {
        $ToolArgs += @('--language', 'en')
    }
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Exe
    # All test arguments are controlled strings without embedded quotes.
    $start.Arguments = ($ToolArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.WorkingDirectory = $runRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = $utf8
    $start.StandardErrorEncoding = $utf8
    $start.EnvironmentVariables['ProgramData'] = Join-Path $runRoot 'ProgramData'
    $start.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $runRoot 'UserProfile'
    $start.EnvironmentVariables['TOKEN_REQUEST_TEST_PASSWORD'] = $password
    $start.EnvironmentVariables['PATH'] = (Split-Path -Parent $OpenSslExe) + ';' + $env:PATH
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $inputBytes = $utf8.GetBytes($InputText)
        $process.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Test child exceeded 30 seconds'
        }
        return [pscustomobject]@{
            Code = $process.ExitCode
            Output = $stdout.Result + $stderr.Result
        }
    } finally {
        $process.Dispose()
    }
}

try {
    # A Unicode path with spaces exercises the file APIs used by Save/Open.
    $requestDir = Join-Path $runRoot (([string][char]0x041F) + ' request space')
    [void][IO.Directory]::CreateDirectory($requestDir)
    $requestPath = Join-Path $requestDir 'searchclient-auth-request.json'
    $tokenPath = Join-Path $requestDir 'searchclient-auth-token.json'
    $missingKeys = Join-Path $runRoot 'missing-keys'
    $keys = Join-Path $runRoot 'test-keys'
    $db = Join-Path $runRoot 'auth-test.sqlite'

    $result = Run-Tool $IssuerExe @('--keystore', $missingKeys) "0`n"
    Check ($result.Code -eq 2 -and $result.Output.Contains('3 - Create unsigned')) 'Menu is shown before any key/password prompt; Cancel returns 2'
    Check (-not (Test-Path -LiteralPath $missingKeys)) 'Cancelling the menu creates no signing key'

    $result = Run-Tool $IssuerExe @('--language', 'auto', '--keystore', $missingKeys) "`n0`n"
    Check ($result.Code -eq 2 -and $result.Output.Contains('Операции с токенами:') -and -not $result.Output.Contains('Token operation:')) 'Default language is Russian, with no English menu'
    $result = Run-Tool $IssuerExe @('--language', 'auto', '--keystore', $missingKeys) "7`n2`n0`n"
    Check ($result.Code -eq 2 -and $result.Output.Contains('Enter 1 or 2.') -and $result.Output.Contains('Token operation:') -and -not $result.Output.Contains('Операции с токенами:')) 'Invalid language is retried and English selection is honored'
    $result = Run-Tool $IssuerExe @('--language', 'xx', '--keystore', $missingKeys)
    Check ($result.Code -eq 1 -and -not (Test-Path -LiteralPath $missingKeys)) 'Unknown language fails before key creation'
    $result = Run-Tool $IssuerExe @('--console-wrapper', '/quiet', '--language', 'ru', '--keystore', $missingKeys) "0`n"
    Check ($result.Code -eq 2 -and $result.Output.Contains('Операция отменена.') -and -not $result.Output.Contains('Operation cancelled.')) 'BAT wrapper preserves Cancel code and localizes completion'

    foreach ($badArgs in @(
        @('--name', '   ', '--id', 'TEST', '--output', $tokenPath),
        @('--name', 'Test User', '--id', '', '--output', $tokenPath),
        @('--name', 'Test User', '--id', 'TEST', '--output', '   ')
    )) {
        $result = Run-Tool $IssuerExe (@('--device-type', 'computer', '--keystore', $missingKeys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD', '--yes') + $badArgs)
        Check ($result.Code -eq 1 -and $result.Output.Contains('must be non-empty') -and -not (Test-Path -LiteralPath $missingKeys) -and -not (Test-Path -LiteralPath $tokenPath)) 'Blank CLI identity/output is rejected before creating keys or token'
    }
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--create-request', '--name', '   ', '--output', $requestPath)
    Check ($result.Code -eq 1 -and $result.Output.Contains('Обязательное поле client_name') -and -not (Test-Path -LiteralPath $requestPath)) 'Russian request validation rejects whitespace-only name'

    $result = Run-Tool $IssuerExe @('--create-request', '--name', 'Test Remote User', '--output', $requestDir, '--yes')
    Check ($result.Code -eq 0) 'Unsigned request is generated for the current computer'
    Check (-not (Test-Path -LiteralPath (Join-Path $runRoot 'ProgramData'))) 'Request creation never initializes a keystore'
    $request = Get-Content -LiteralPath $requestPath -Raw | ConvertFrom-Json
    Check ($request.format -eq 'searchclient-auth-request' -and $request.device_type -eq 'computer') 'Request has a distinct unsigned format'
    Check (-not ($request.PSObject.Properties.Name -contains 'signature')) 'Request contains no signature'
    Check ($request.client_id -eq ('PC-' + $request.device_id)) 'Default client ID is tied to the requesting PC, not C-001'
    $requestHash = File-Hash $requestPath
    $result = Run-Tool $IssuerExe @('--create-request', '--name', 'Other User', '--output', $requestPath) "n`n"
    Check ($result.Code -eq 2 -and (File-Hash $requestPath) -eq $requestHash) 'Declining overwrite preserves the request'

    # AuthDbTool has a narrow argv entrypoint. Its existing import contract is
    # checked using an ASCII path, independently of the issuer's Unicode paths.
    $asciiRequest = Join-Path $runRoot 'unsigned-request.json'
    Copy-Item -LiteralPath $requestPath -Destination $asciiRequest
    $result = Run-Tool $AuthDbExe @('--db', $db, 'add-from-token', '--token', $asciiRequest)
    Check ($result.Code -ne 0 -and $result.Output.Contains('token format must be')) 'AuthDbTool refuses to register the unsigned request'
    $result = Run-Tool $IssuerExe @('--sign-request', $requestPath, '--output', $tokenPath, '--keystore', $missingKeys, '--yes')
    Check ($result.Code -ne 0 -and -not (Test-Path -LiteralPath $missingKeys) -and -not (Test-Path -LiteralPath $tokenPath)) 'Signing without an existing key fails without creating a key or token'
    $result = Run-Tool $IssuerExe @('--create-request', '--sign-request', $requestPath, '--output', $tokenPath)
    Check ($result.Code -ne 0 -and -not (Test-Path -LiteralPath $tokenPath)) 'Conflicting request modes are rejected'

    $result = Run-Tool $IssuerExe @('--init-keystore', '--keystore', $keys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD')
    Check ($result.Code -eq 0) 'Disposable signing keystore is initialized explicitly'
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--keystore', $keys) ("2`n`n  `n" + $password + "`n`n  `nИванов`nActual User`n`n  `nLOCAL-TEST`n`n`n8`n1`n")
    $localTokenPath = Join-Path $runRoot 'UserProfile\SearchEngine\searchclient-auth-token.json'
    Check (-not (Test-Path -LiteralPath (Join-Path $runRoot 'ProgramData\SearchEngine\searchclient-auth-token.json'))) 'Interactive token never writes to shared ProgramData'
    Check ($result.Code -eq 0 -and (Test-Path -LiteralPath $localTokenPath)) 'Interactive computer token is saved after corrected required input'
    Check (([regex]::Matches($result.Output, 'Обязательное поле:')).Count -eq 4) 'Empty and whitespace-only name and ID each cause a retry'
    Check (([regex]::Matches($result.Output, 'Пароль не должен быть пустым')).Count -eq 2) 'Existing-keystore password rejects empty and whitespace-only input'
    Check ($result.Output.Contains('без кириллицы и кавычек') -and $result.Output.Contains('Введите 1, 2 или 0.')) 'Invalid identity characters and save selection are rejected in Russian'
    Check (-not $result.Output.Contains('Ivanov I.I.') -and -not $result.Output.Contains('C-001') -and -not $result.Output.Contains('Keystore password:') -and -not $result.Output.Contains('Token written:')) 'Russian interaction contains no English prompts or demo identity defaults'
    $localToken = Get-Content -LiteralPath $localTokenPath -Raw | ConvertFrom-Json
    Check ($localToken.client_name -eq 'Actual User' -and $localToken.client_id -eq 'LOCAL-TEST' -and $localToken.notes -eq '') 'Token stores entered identity and permits explicitly optional empty notes'
    $localBytes = [IO.File]::ReadAllBytes($localTokenPath)
    $reusePath = Join-Path $runRoot 'reused-request.json'
    $result = Run-Tool $IssuerExe @('--create-request', '--output', $reusePath, '--yes')
    $reuse = Get-Content -LiteralPath $reusePath -Raw | ConvertFrom-Json
    Check ($result.Code -eq 0 -and $reuse.client_name -eq 'Actual User' -and $reuse.client_id -eq 'LOCAL-TEST') 'Existing local token supplies name and ID without a prompt'
    Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($localTokenPath)) -eq [Convert]::ToBase64String($localBytes)) 'Creating a request preserves the installed token bytes'
    [IO.File]::Delete($localTokenPath)
    $result = Run-Tool $IssuerExe @('--create-request', '--output', $reusePath, '--yes')
    $reuse = Get-Content -LiteralPath $reusePath -Raw | ConvertFrom-Json
    Check ($result.Code -eq 0 -and $reuse.client_name -eq 'Actual User') 'Saved request supplies the name when no token exists'
    [IO.File]::WriteAllBytes($localTokenPath, $localBytes)
    $legacy = $localToken | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $legacy.client_id = 'local-machine'
    [IO.File]::WriteAllText($localTokenPath, ($legacy | ConvertTo-Json -Depth 8), $utf8)
    $result = Run-Tool $IssuerExe @('--create-request', '--output', $reusePath, '--yes')
    $reuse = Get-Content -LiteralPath $reusePath -Raw | ConvertFrom-Json
    Check ($result.Code -eq 0 -and $reuse.client_id -eq ('PC-' + $reuse.device_id) -and $reuse.client_name -eq 'Actual User') 'Legacy local-machine request gets a unique ID while retaining its name'
    $legacy.device_id = 'ABCDEF12-1234-1234-1234-123456789ABC'
    [IO.File]::WriteAllText($localTokenPath, ($legacy | ConvertTo-Json -Depth 8), $utf8)
    $result = Run-Tool $IssuerExe @('--create-request', '--output', $reusePath, '--yes')
    Check ($result.Code -ne 0 -and $result.Output.Contains('another device')) 'Existing token for another machine is refused'
    [IO.File]::WriteAllBytes($localTokenPath, $localBytes)

    $result = Run-Tool $IssuerExe @('--language', 'en', '--keystore', $keys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD') "2`n`nEnglish User`n`nEN-TEST`n`n`n0`n"
    Check ($result.Code -eq 2 -and ([regex]::Matches($result.Output, 'Value must be non-empty')).Count -eq 2 -and -not $result.Output.Contains('Обязательное поле:')) 'English required fields retry in English and save can be cancelled'
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--keystore', $keys) "2`nwrong-test-password`n"
    Check ($result.Code -eq 1 -and $result.Output.Contains('неверный пароль')) 'Wrong keystore password has a Russian explanation'
    $freshKeys = Join-Path $runRoot 'interactive-new-keys'
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--init-keystore', '--keystore', $freshKeys) ("`n`n  `n  `n" + $password + "`n" + $password + "`n")
    Check ($result.Code -eq 0 -and ([regex]::Matches($result.Output, 'Пароль не должен быть пустым')).Count -eq 2) 'New keystore also rejects empty and whitespace-only passwords'
    # Simulate a request brought from another PC. Signing must not replace its UUID.
    $remoteUuid = 'A1B2C3D4-E5F6-7890-ABCD-EF1234567890'
    if ($request.device_id -eq $remoteUuid) { $remoteUuid = 'B1B2C3D4-E5F6-7890-ABCD-EF1234567890' }
    $request.device_id = $remoteUuid
    [IO.File]::WriteAllText($requestPath, ($request | ConvertTo-Json), $utf8)
    $requestHash = File-Hash $requestPath
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--sign-request', $requestPath, '--output', $tokenPath, '--keystore', $keys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD') "Н`n"
    Check ($result.Code -eq 2 -and -not (Test-Path -LiteralPath $tokenPath)) 'Russian No cancels signing without writing a token'
    $result = Run-Tool $IssuerExe @('--language', 'ru', '--sign-request', $requestPath, '--output', $tokenPath, '--keystore', $keys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD') "Д`n"
    Check ($result.Code -eq 0 -and $result.Output.Contains('Токен сохранён:') -and -not $result.Output.Contains('Token written:')) 'Russian Yes signs the remote request with localized output'
    $signArgs = @('--sign-request', $requestPath, '--keystore', $keys, '--password-env', 'TOKEN_REQUEST_TEST_PASSWORD', '--yes')
    $result = Run-Tool $IssuerExe ($signArgs + @('--output', $requestPath))
    Check ($result.Code -ne 0 -and (File-Hash $requestPath) -eq $requestHash) 'Signing cannot overwrite its input, even with --yes'
    $result = Run-Tool $IssuerExe ($signArgs + @('--output', $requestDir))
    Check ($result.Code -eq 0) 'Request is signed into the chosen directory'
    Check ((File-Hash $requestPath) -eq $requestHash) 'Original request remains unchanged after signing'
    $token = Get-Content -LiteralPath $tokenPath -Raw | ConvertFrom-Json
    Check ($token.format -eq 'searchclient-auth-token' -and $token.format_version -eq 1 -and $token.device_id -eq $remoteUuid) 'Signed token retains the remote UUID and existing token format'

    $payloadPath = Join-Path $runRoot 'payload.txt'
    $signaturePath = Join-Path $runRoot 'signature.bin'
    $payload = $token.client_id + "`n" + $token.client_name + "`ncomputer`n" + $token.device_id + "`n"
    [IO.File]::WriteAllText($payloadPath, $payload, $utf8)
    [IO.File]::WriteAllBytes($signaturePath, [Convert]::FromBase64String($token.signature.value))
    $verifyArgs = @('dgst', '-sha256', '-verify', (Join-Path $keys 'public.pem'), '-signature', $signaturePath, $payloadPath)
    $result = Run-Tool $OpenSslExe $verifyArgs
    Check ($result.Code -eq 0) 'Independent RS256 verification succeeds for the server identity payload'
    [IO.File]::WriteAllText($payloadPath, ('Changed' + $payload), $utf8)
    $result = Run-Tool $OpenSslExe $verifyArgs
    Check ($result.Code -ne 0) 'Changing signed identity fields invalidates the signature'
    $asciiToken = Join-Path $runRoot 'signed-token.json'
    Copy-Item -LiteralPath $tokenPath -Destination $asciiToken
    $result = Run-Tool $AuthDbExe @('--db', $db, 'add-from-token', '--token', $asciiToken)
    Check ($result.Code -ne 0) 'Registration refuses a token until the issuer public key is available'
    $result = Run-Tool $IssuerExe @('--export-public', $runRoot, '--keystore', $keys)
    Check ($result.Code -eq 0) 'Authority public key is exported beside the test server database'
    $tampered = $token | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $tampered.client_name = 'Tampered'
    $tamperedPath = Join-Path $runRoot 'tampered-token.json'
    [IO.File]::WriteAllText($tamperedPath, ($tampered | ConvertTo-Json -Depth 8), $utf8)
    $result = Run-Tool $AuthDbExe @('--db', $db, 'add-from-token', '--token', $tamperedPath)
    Check ($result.Code -ne 0 -and $result.Output.Contains('signature')) 'Registration rejects changed signed fields'
    $result = Run-Tool $AuthDbExe @('--db', $db, 'add-from-token', '--token', $asciiToken)
    Check ($result.Code -eq 0) 'AuthDbTool verifies and imports the signed token'
    $result = Run-Tool $AuthDbExe @('--db', $db, 'get', '--id', $token.client_id)
    Check ($result.Code -eq 0 -and $result.Output.Contains($remoteUuid)) 'Registration stores the remote computer identity'
    $result = Run-Tool $IssuerExe @('--sign-request', $tokenPath, '--output', (Join-Path $runRoot 'unexpected.json'), '--keystore', $keys, '--yes')
    Check ($result.Code -ne 0) 'An already signed token is not accepted as a request'
    $result = Run-Tool $AuthDbExe @('--db', $db, 'disable', '--id', $token.client_id)
    Check ($result.Code -eq 0) 'Client can be explicitly disabled'
    $result = Run-Tool $AuthDbExe @('--db', $db, 'add-from-token', '--token', $tokenPath)
    Check ($result.Code -eq 0) 'Verified registration supports a Unicode token path'
    $result = Run-Tool $AuthDbExe @('--db', $db, 'get', '--id', $token.client_id)
    Check ($result.Output.Contains('disabled')) 'Reimport does not silently enable a disabled client'
    Write-Host 'Token request CLI workflow passed.'
} finally {
    # Only remove this run's generated files, after resolving the containment.
    $resolvedRun = [IO.Path]::GetFullPath($runRoot)
    $boundary = $workBase.TrimEnd('\') + '\'
    if (-not $resolvedRun.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the test workspace'
    }
    Remove-Item -LiteralPath $resolvedRun -Recurse -Force
}
