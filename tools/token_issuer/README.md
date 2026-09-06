# SearchClientTokenIssuer

Windows CLI that issues `searchclient-auth-token.json` for SearchClient
device authentication (`device_type` = `usb` or `computer`).

The standard PC-token location is per Windows user:
`%LOCALAPPDATA%\SearchEngine\searchclient-auth-token.json`.
SearchClient uses only this location for automatic PC-token lookup, with no
ProgramData or executable-directory fallback. Existing signed tokens can be
moved there manually without signing or registration again. Run/copy under the
intended Windows account; a process started as another user uses that user's
profile. Both issuer and client must be updated. The signing keystore remains
at its configured location.

The BAT launcher and the default interactive mode first offer a language menu:
Enter/1 selects Russian, 2 selects English. Use `--language ru|en|auto` to
select explicitly. Command-line operations default to English without an extra
prompt; the BAT accepts `/quiet` to skip language selection and the final pause.

Interactive name and client ID require explicit non-blank input; template
examples are never accepted by pressing Enter. Passwords cannot be empty or
whitespace-only. Invalid values are requested again. Issuer and notes are
explicitly optional; Enter keeps their displayed value. Token identity fields
remain printable ASCII regardless of the interface language.

## Offline computer enrollment

Run `Issue-SearchClientToken.bat` from the portable package on the client PC
and choose **3 - Create unsigned token for this computer (request)**. Enter
the client name in Latin letters and choose a folder in the Save dialog.
The default client ID is `PC-<SMBIOS UUID>`, avoiding a shared `C-001` default.
This operation never creates or unlocks a keystore.

Take `searchclient-auth-request.json` to the authorized signing computer.
Choose **4 - Sign a received computer request**, open the request, review its
identity, confirm signing, select the output location, and enter the existing
issuer keystore password. Signing never creates a replacement key and never
substitutes the signing computer's UUID. The input request is preserved.

Register the resulting `searchclient-auth-token.json` using
`Register-AuthClient-FromToken.bat` (select the server instance, then option 3
to select the file manually), and copy the signed token to the requesting PC's
`%LOCALAPPDATA%\SearchEngine\searchclient-auth-token.json`. Restart SearchClient
and connect to the server. Keep the private signing key on the administrator's
computer; do not include the `keys` directory when distributing the utility.

The unsigned file uses `format: searchclient-auth-request`, version 1, and only
`client_id`, `client_name`, `device_type`, and `device_id`. It has no signature
and is rejected by the existing client and AuthDbTool token loaders. The signed
token format and network protocol are unchanged.

```powershell
.\SearchClientTokenIssuer.exe --create-request --name "Ivanov" --output D:\transfer
.\SearchClientTokenIssuer.exe --sign-request D:\transfer\searchclient-auth-request.json `
  --output D:\transfer --keystore D:\issuer\keys
```

`--output` accepts an existing directory or an explicit file path. Omit it for
a Save dialog. `--create-request` also accepts an optional `--id`; signing keeps
the identity in the request. `--yes` skips confirmation/overwrite prompts;
`--password-env NAME` supplies the signing password for automation.

User instructions in Russian: [offline enrollment](../../tutorials/REMOTE_PC_TOKEN.md).

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

Interactive registration (portable package or repo):

```powershell
.\Register-AuthClient-FromToken.bat
```

After selecting the service instance, the helper offers:

```text
1 - Computer token   (%LOCALAPPDATA%\SearchEngine\searchclient-auth-token.json)
2 - USB token        (searchclient-auth-token.json on removable drives)
3 - Select token file manually
```

Non-interactive:

```powershell
.\AuthDbTool.exe --db <data>\auth_clients.sqlite add-from-token --token <path>
# or
.\scripts\Register-AuthClientFromToken.ps1 -TokenPath <path>
```

When `-TokenPath` or BAT `/token <path>` is supplied, the source menu is skipped.

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
