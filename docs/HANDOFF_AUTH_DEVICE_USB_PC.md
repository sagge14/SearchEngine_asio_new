# SearchEngine AUTH — HANDOFF: USB + PC device tokens

## Scope

Repository: `sagge14/SearchEngine_asio_new`

Base branch: `feat/authenticate-v1-sqlite`

This document is the source task for the **server/admin side** of the transition from USB-only `flash_serial` authorization to a unified device model.

Do **not** modify `myLitleWork/SearchEngine-client` in this task.

The AUTHENTICATE_V1 scheme has not been released yet, so backward compatibility with the old `flash_serial` token format is **not required**.

Target identity model:

```text
device_type
device_id
```

Supported device types:

```text
usb
computer
```

USB `device_id` = existing USB hardware serial.

Computer `device_id` = normalized `Win32_ComputerSystemProduct.UUID` (SMBIOS UUID).

---

## 1. Components in this repository that must be changed together

Implement the whole server/admin side as one coordinated change:

1. `SearchClientTokenIssuer.exe`
2. `AuthDbTool.exe`
3. `SearchEngine.exe` / `SearchEngineService` auth runtime, AUTHENTICATE_V1 and auth DB schema
4. `Register-AuthClientFromToken` helper chain
5. CMake / SearchEngine portable package

Do not touch BackupService, BackupRestore, ZagEditor, PRM/PRD search behavior, or unrelated code.

---

## 2. Unified token format

Completely remove `flash_serial` from the new auth contract.

Use one token format for USB and PC:

```json
{
  "format": "searchclient-auth-token",
  "format_version": 1,
  "client_id": "...",
  "client_name": "...",
  "device_type": "usb",
  "device_id": "...",
  "signature": {
    "alg": "RS256",
    "encoding": "base64",
    "value": "..."
  }
}
```

For a PC token:

```json
{
  "format": "searchclient-auth-token",
  "format_version": 1,
  "client_id": "...",
  "client_name": "...",
  "device_type": "computer",
  "device_id": "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX",
  "signature": {
    "alg": "RS256",
    "encoding": "base64",
    "value": "..."
  }
}
```

Do not keep parallel legacy v2/v3 auth paths. One clean pre-release format is preferred.

---

## 3. Device identity rules

### USB

Keep and reuse the existing implementation:

```text
tools/token_issuer/VolumeSerial.*
```

`device_id` is the current normalized hardware serial of the removable USB device.

Do not rewrite the working USB WMI code unless required for the new model.

### Computer

Add a small dedicated module, for example:

```text
tools/token_issuer/ComputerIdentity.hpp
tools/token_issuer/ComputerIdentity.cpp
```

Obtain:

```text
Win32_ComputerSystemProduct.UUID
```

through WMI.

Normalize it as:

- trim surrounding whitespace;
- uppercase;
- canonical UUID representation.

Reject at least:

```text
empty
00000000-0000-0000-0000-000000000000
FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF
```

Do not silently fall back to:

- MAC address;
- hostname;
- Windows username;
- MachineGuid;
- random identifiers.

If a usable SMBIOS UUID cannot be obtained, token issuance must fail with a clear error.

TPM / hardware-backed attestation is outside this task.

---

## 4. Signing contract

TokenIssuer and SearchEngine must sign/verify **exactly the same bytes**:

```text
client_id + '\n'
client_name + '\n'
device_type + '\n'
device_id + '\n'
```

The final newline is mandatory.

Update both IdentitySigning implementations accordingly.

Update server `AuthIdentity` to contain:

```text
client_id
client_name
device_type
device_id
```

Update `RsaIdentitySignatureVerifier` to verify the new signing payload.

Keep the existing RSA/RS256 issuer key architecture.

One issuer keypair is used for both USB and PC tokens.

Never copy `private.enc.pem` to the server package or client package.

---

## 5. SearchClientTokenIssuer.exe

Extend the existing executable; do not create a second PC issuer program.

Interactive mode should support:

```text
Token type:
1 - USB
2 - Computer
```

### USB mode

- preserve the existing removable-volume selection flow;
- obtain the USB hardware serial;
- set `device_type=usb`;
- set `device_id=<serial>`;
- keep the convenient behavior of writing the token to the USB root when appropriate.

### Computer mode

- automatically obtain current `Win32_ComputerSystemProduct.UUID`;
- normalize and validate it;
- show the UUID to the operator before issuance;
- set `device_type=computer`;
- set `device_id=<UUID>`;
- use `%ProgramData%\SearchEngine\searchclient-auth-token.json` as the standard PC-token location;
- in interactive mode, propose `%ProgramData%\SearchEngine\searchclient-auth-token.json` as the default output path and create the `SearchEngine` directory when needed;
- allow an explicit output path to override the default location;
- keep `searchclient-auth-token.json` next to `SearchEngine-client.exe` as the defined client-side fallback location for a later client synchronization task.

The client-side lookup contract is therefore fixed now, even though `SearchEngine-client` is not modified in this server/admin task:

```text
1. %ProgramData%\SearchEngine\searchclient-auth-token.json
2. <SearchEngine-client.exe directory>\searchclient-auth-token.json
```

The subsequent `SearchEngine-client` task must search PC tokens in exactly that order. USB-token discovery remains separate and must have priority over a PC token.

CLI should support roughly:

```text
--device-type usb --drive E: --name ... --id ...
--device-type computer --name ... --id ... --output <path>
```

For non-interactive Computer issuance, `--output` remains available as an explicit override. If no explicit output is supplied in a mode where prompting is possible, use/propose the standard `%ProgramData%\SearchEngine\searchclient-auth-token.json` path.

Add a diagnostic mode such as:

```text
--show-computer-id
```

which prints the normalized computer UUID and issues no token.

Preserve existing keystore operations such as:

```text
--init-keystore
--export-public
```

Remove obsolete flash-specific CLI/API where it no longer makes sense after the redesign.

---

## 6. AuthDbTool.exe

Move the CLI and token parser completely from flash-specific terminology to:

```text
--device-type
--device-id
```

For example:

```text
add --id ... --name ... --device-type usb --device-id ...
update ...
```

`add-from-token` must parse the new unified USB/PC token format.

`list` and `get` must show at least:

```text
client_id
client_name
device_type
device_id
enabled
```

Do not support old `flash_serial` token JSON as a compatibility path.

---

## 7. auth_clients.sqlite schema

Replace the flash-specific record with a generic device record.

Target schema conceptually:

```sql
client_id TEXT PRIMARY KEY NOT NULL,
client_name TEXT NOT NULL,
device_type TEXT NOT NULL,
device_id TEXT NOT NULL,
enabled INTEGER NOT NULL DEFAULT 1,
signature_meta TEXT NOT NULL DEFAULT '',
created_at INTEGER NOT NULL,
updated_at INTEGER NOT NULL
```

Remove `flash_serial` from the final schema/API.

Lookup/indexing must use:

```text
client_id
client_name
device_type
device_id
enabled
```

Because this is pre-release, backward-compatible migration is not required.

However, an old schema must **not** be silently mistaken for the new schema. Use deterministic behavior: either perform an intentional schema replacement/migration, or reject the old DB with a clear message telling the operator to recreate `auth_clients.sqlite`.

Do not allow late runtime failures due to missing columns.

---

## 8. AUTHENTICATE_V1

Keep wire command ID unchanged:

```text
AUTHENTICATE_V1 = 31
```

New request body:

```json
{
  "client_id": "...",
  "client_name": "...",
  "device_type": "usb|computer",
  "device_id": "...",
  "signature": "..."
}
```

Validation order should remain explicit and deterministic:

```text
valid JSON
client_id present
client_name present
device_type present
device_id present
signature present
client_id exists
enabled
client_name matches
device_type matches
device_id matches
signature valid
authenticated = true
```

Do not accept `flash_serial` as a fallback.

The server does not attempt to discover the remote PC hardware itself. It verifies registered identity fields and the RSA signature.

AUTHENTICATE_V1 must remain usable by remote network clients.

---

## 9. Auth ErrorCode block

Because the protocol has not been released, normalize the auth block to the generic device model.

Preferred final mapping:

```text
33 AuthFailed
34 AuthClientDisabled
35 AuthRequired
36 AuthClientIdMissing
37 AuthClientNameMissing
38 AuthDeviceTypeMissing
39 AuthDeviceIdMissing
40 AuthSignatureMissing
41 AuthClientIdNotFound
42 AuthClientNameMismatch
43 AuthDeviceTypeMismatch
44 AuthDeviceIdMismatch
45 AuthSignatureInvalid
```

All numeric values must remain explicit in the enum.

Remove the flash-specific symbolic names:

```text
AuthFlashSerialMissing
AuthFlashSerialMismatch
```

After implementation, report the exact final numeric table actually committed. The subsequent SearchEngine-client task will mirror the server's actual final contract.

Do not add unrelated datasource error codes in this task.

---

## 10. ASIO session state and legacy admin

Audit session state for flash-specific members such as:

```text
flashSerial_
```

and make them device-neutral where they are part of AUTHENTICATE_V1/session state.

Do **not** weaken or redesign the already implemented legacy admin gate:

```text
USER_REGISTRY("admin")
```

must authorize only when the TCP **remote peer** is exactly IPv4 `127.0.0.1`.

Do not allow `::1`, other `127/8` addresses, LAN addresses, hostnames, payload IPs, or endpoint lookup failure.

AUTHENTICATE_V1 remains a separate remote-capable path.

---

## 11. Register-AuthClientFromToken helper

Update both:

```text
scripts/Register-AuthClientFromToken.ps1
```

and:

```text
deployment/SearchEngineServicePortable/Register-AuthClient-FromToken-Windows7.bat
```

Make the UX neutral to token type: say `auth token`, not `USB auth token`.

A token path may point to:

- a USB token;
- a local PC token;
- any explicitly selected valid token JSON.

Do not require `E:\` as the only location. It is acceptable to preserve `E:\searchclient-auth-token.json` as a convenient detected default, provided the user can choose another file.

Preserve:

- installed SearchEngineService instance selection;
- data-dir resolution;
- invocation of `AuthDbTool add-from-token`;
- Windows 7 / PowerShell compatibility already present.

Do not modify `SearchEngineConfig.exe` unless a real dependency is discovered. Its role here is only installed-service selection.

---

## 12. CMake / build / portable package

Update `CMakeLists.txt` so new ComputerIdentity sources are actually compiled into `SearchClientTokenIssuer.exe`.

Update USB-only target descriptions/comments where appropriate.

Audit:

```text
CMakeLists.txt
scripts/New-SearchEngineServicePackage.ps1
deployment/SearchEngineServicePortable/*
```

The portable package must continue to include current versions of:

```text
SearchEngine.exe
SearchEngineConfig.exe
AuthDbTool.exe
SearchClientTokenIssuer.exe
searchclient-auth-token.defaults.json
Register-AuthClientFromToken.ps1
required OpenSSL runtime DLL(s)
```

Never package:

```text
private.enc.pem
personal USB tokens
personal PC tokens
```

If server-side README/install text explicitly says tokens are USB-only, update it narrowly. Do not perform unrelated documentation refactoring.

---

## 13. Tests

Add/update regression coverage for at least:

1. exact signing bytes: `client_id\nclient_name\ndevice_type\ndevice_id\n`;
2. USB device identity stored/read from auth DB;
3. Computer device identity stored/read from auth DB;
4. USB token contains `device_type=usb` and correct `device_id`;
5. Computer token contains `device_type=computer` and normalized UUID;
6. valid computer UUID normalization;
7. zero UUID rejected;
8. all-`F` UUID rejected;
9. missing `device_type` => `AuthDeviceTypeMissing`;
10. missing `device_id` => `AuthDeviceIdMissing`;
11. unknown `client_id` => `AuthClientIdNotFound`;
12. `device_type` mismatch => `AuthDeviceTypeMismatch`;
13. `device_id` mismatch => `AuthDeviceIdMismatch`;
14. signature tamper => `AuthSignatureInvalid`;
15. valid USB AUTHENTICATE_V1 succeeds;
16. valid PC AUTHENTICATE_V1 succeeds;
17. legacy flash_serial-only request no longer authenticates;
18. old auth DB schema handling is deterministic;
19. localhost legacy admin regression remains exact `127.0.0.1` only;
20. remote AUTHENTICATE_V1 remains allowed.

If some hardware/WMI tests cannot be made deterministic, isolate pure normalization/validation from WMI access so the pure logic is unit-testable.

---

## 14. Explicit non-goals

Do not modify:

```text
myLitleWork/SearchEngine-client
BackupService
BackupRestore
ZagEditor
PRM/PRD/search functionality
```

The PC-token lookup locations are nevertheless part of the agreed cross-repository contract for the later client task:

```text
1. %ProgramData%\SearchEngine\searchclient-auth-token.json
2. <SearchEngine-client.exe directory>\searchclient-auth-token.json
```

Do not:

- add TPM support;
- change AUTHENTICATE_V1 command ID;
- weaken legacy localhost admin restriction;
- perform unrelated refactoring;
- mix in other pending server feature work.

---

## 15. Build and verification

Run as much as the environment allows:

- auth/unit tests;
- SearchEngine build;
- AuthDbTool build;
- SearchClientTokenIssuer build;
- SearchEngine portable package verification.

Check relevant Windows targets/configurations where available, especially x64/x86/Windows7 paths already supported by the project.

If a build/test cannot be run in the current environment, report it as **not run**, not as passed.

---

## 16. Commit requirements for the implementation task

Implementation should be performed in a separate feature branch created from the current HEAD of `feat/authenticate-v1-sqlite`, for example:

```text
feat/auth-device-usb-pc
```

Do not push implementation changes directly into the base branch or `main`.

After implementation:

- inspect the full diff against the base HEAD;
- verify no unrelated changes;
- run available tests/build/package verification;
- make one final implementation commit:

```text
feat(auth): unify USB and computer device tokens
```

Push the feature branch only. Do not merge it automatically.

---

## 17. Required final implementation report

Report:

1. base branch and base HEAD used;
2. feature branch name;
3. implementation commit SHA;
4. changed files;
5. final token JSON;
6. exact signing payload bytes;
7. final `auth_clients.sqlite` schema;
8. final AUTHENTICATE_V1 JSON;
9. exact final ErrorCode mapping;
10. how USB `device_id` is obtained;
11. how computer `device_id` is obtained;
12. exact PC-token output/default location behavior in `SearchClientTokenIssuer`;
13. Register-AuthClientFromToken behavior;
14. build/test/package results;
15. exact client-side changes still required in `SearchEngine-client` to synchronize with the committed server wire contract, including PC-token lookup order, USB priority, and fallback to a valid PC token after USB removal.

After that implementation commit, the server becomes the source of truth for the subsequent SearchEngine-client synchronization task.
