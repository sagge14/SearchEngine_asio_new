# SearchClientTokenIssuer

Windows CLI that issues `searchclient-auth-token.json` for SearchClient
device authentication (`device_type` = `usb` or `computer`).

## Crypto / signature

- **RSA-2048 keystore** (`--init-keystore` or first issue). Private key:
  PKCS#8 AES-256-CBC (`private.enc.pem`). Default:
  `%ProgramData%\SearchClientTokenIssuer\keys\`.
- Issued tokens are **`format_version: 1`** with `signature.alg: "RS256"`.
- Signed message (UTF-8):
  `client_id + "\n" + client_name + "\n" + device_type + "\n" + device_id + "\n"`.
- **Server** verifies RS256 with `issuer-public.pem` next to
  `auth_clients.sqlite`, or `%ProgramData%\SearchClientTokenIssuer\keys\public.pem`
  when the sibling export is missing. **Client** checks the local device identity and forwards
  `signature.value` in `AUTHENTICATE_V1`.
- Export public key into the service data directory:

```powershell
.\SearchClientTokenIssuer.exe --export-public "$env:ProgramData\SearchEngineService"
# writes ...\issuer-public.pem
```

- Token string fields are printable ASCII only (no Cyrillic).
- `expires_at` is not enforced yet.

## Register after issue

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

Portable packages ship `tools\SearchClientTokenIssuer.exe`, defaults JSON, and
OpenSSL `libcrypto` DLL beside the tool.

## Usage

```powershell
.\SearchClientTokenIssuer.exe --init-keystore --password-env TOKEN_ISSUER_PASSWORD
.\SearchClientTokenIssuer.exe --export-public "C:\ProgramData\SearchEngineService"
.\SearchClientTokenIssuer.exe --show-computer-id
.\SearchClientTokenIssuer.exe --device-type usb --drive E: --name "Ivanov I.I." --id C-001 --yes `
  --password-env TOKEN_ISSUER_PASSWORD
.\SearchClientTokenIssuer.exe --device-type computer --name "Ivanov I.I." --id C-001 `
  --output D:\tokens\searchclient-auth-token.json --yes `
  --password-env TOKEN_ISSUER_PASSWORD
```

| Flag | Meaning |
|------|---------|
| `--init-keystore` | Create RSA-2048 keystore |
| `--export-public PATH` | Copy `public.pem` to file or `issuer-public.pem` in a directory |
| `--show-computer-id` | Print normalized SMBIOS UUID; issue no token |
| `--device-type usb\|computer` | Token type |
| `--drive E:` | USB volume (USB tokens) |
| `--output PATH` | Token file or directory (required for computer tokens) |
| `--defaults PATH` | Token defaults JSON |
| `--keystore PATH` | Keystore directory |
| `--password-env NAME` | Keystore password from environment |
| `--allow-manual-serial S` | Override USB hardware serial (warning) |
| `--yes` | Overwrite without confirm |

Exit codes: `0` ok, `1` error, `2` cancelled, `3` no eligible volume/serial.
