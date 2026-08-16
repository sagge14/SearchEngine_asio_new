# SearchClientTokenIssuer

Windows CLI that issues `searchclient-auth-token.json` onto a removable USB
volume for SearchClient flash authentication.

## Stage 1 notes

- **RSA-2048 keystore** is generated locally (`--init-keystore` or first issue).
  Private key is stored as PKCS#8 encrypted with AES-256-CBC (`private.enc.pem`).
  Default directory: `%ProgramData%\SearchClientTokenIssuer\keys\`.
- **Token `signature.alg` is still `"none"`** until client/server rollout.
- **Token string fields are printable ASCII only** (`client_name`, `client_id`,
  `issuer`, `notes`, …). Allowed extras: space `. , - _ ( ) / + # @ :`.
  Cyrillic and other non-ASCII values are rejected.
  Console prompts may still be Russian; JSON on the stick is ASCII.
- Server registration is **not** performed by this tool. After writing the
  token, register with:

```powershell
.\AuthDbTool.exe --db <data>\auth_clients.sqlite add-from-token --token E:\searchclient-auth-token.json
# or
.\scripts\Register-AuthClientFromToken.ps1 -TokenPath E:\searchclient-auth-token.json
```

## Build

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --target SearchClientTokenIssuer
```

Portable SearchEngineService packages ship `tools\SearchClientTokenIssuer.exe`
and `tools\searchclient-auth-token.defaults.json` (built by
`Build-SearchEngineServicePackage.ps1` / PostBuild packaging).

## Usage

Interactive (lists removable volumes with a hardware serial):

```powershell
.\SearchClientTokenIssuer.exe
```

Non-interactive:

```powershell
.\SearchClientTokenIssuer.exe --drive E: --name "Ivanov I.I." --id C-001 --yes `
  --keystore "$env:TEMP\token-issuer-keys" `
  --password-env TOKEN_ISSUER_PASSWORD
```

Options:

| Flag | Meaning |
|------|---------|
| `--defaults PATH` | Token defaults JSON |
| `--keystore PATH` | Stub keystore directory (default `%ProgramData%\SearchClientTokenIssuer\keys` or `keys\` beside EXE) |
| `--password-env NAME` | Read keystore password from environment (never pass password on CLI) |
| `--allow-manual-serial S` | Override hardware serial (prints a warning) |
| `--yes` | Overwrite without confirm |

Exit codes: `0` ok, `1` error, `2` cancelled, `3` no eligible volume/serial.

## Defaults template

`searchclient-auth-token.defaults.json` is copied next to the EXE (and under
`data\`). Missing keys fall back to built-in ASCII samples.
