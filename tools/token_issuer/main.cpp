#include "CryptoStub.hpp"
#include "TokenAscii.hpp"
#include "TokenDefaults.hpp"
#include "TokenDocument.hpp"
#include "IdentitySigning.hpp"
#include "VolumeSerial.hpp"
#include "ComputerIdentity.hpp"
#include "Auth/DeviceIdentity.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using token_issuer::TokenFields;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitCancelled = 2;
constexpr int kExitNoVolume = 3;

std::string Utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("cannot encode text as UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("cannot decode UTF-8 text");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

void WriteOut(const std::wstring& text)
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && output != nullptr &&
        GetConsoleMode(output, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(
            output, text.data(), static_cast<DWORD>(text.size()), &written,
            nullptr);
        return;
    }
    std::cout << Utf8(text);
    std::cout.flush();
}

void WriteErr(const std::wstring& text)
{
    const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && output != nullptr &&
        GetConsoleMode(output, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(
            output, text.data(), static_cast<DWORD>(text.size()), &written,
            nullptr);
        return;
    }
    std::cerr << Utf8(text);
    std::cerr.flush();
}

std::wstring ReadLine()
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && input != nullptr &&
        GetConsoleMode(input, &mode))
    {
        wchar_t buffer[512]{};
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, 511, &read, nullptr)) {
            throw std::runtime_error("cannot read from the console");
        }
        std::wstring result(buffer, buffer + read);
        while (!result.empty() &&
            (result.back() == L'\r' || result.back() == L'\n'))
        {
            result.pop_back();
        }
        return result;
    }

    std::string line;
    if (!std::getline(std::cin, line)) {
        throw std::runtime_error("cannot read interactive input");
    }
    return Wide(line);
}

std::wstring ReadPasswordLine()
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console =
        input != INVALID_HANDLE_VALUE && input != nullptr &&
        GetConsoleMode(input, &mode);
    if (console) {
        SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    }
    std::wstring line;
    try {
        line = ReadLine();
    } catch (...) {
        if (console) {
            SetConsoleMode(input, mode);
        }
        throw;
    }
    if (console) {
        SetConsoleMode(input, mode);
        WriteOut(L"\n");
    }
    return line;
}

void PrintUsage()
{
    WriteErr(
        L"SearchClientTokenIssuer - issue searchclient-auth-token.json\n"
        L"\n"
        L"Interactive (default):\n"
        L"  SearchClientTokenIssuer\n"
        L"    Token type: 1 USB, 2 Computer\n"
        L"\n"
        L"  SearchClientTokenIssuer --init-keystore [--keystore path] "
        L"[--password-env NAME]\n"
        L"  SearchClientTokenIssuer --export-public <file-or-dir> "
        L"[--keystore path]\n"
        L"  SearchClientTokenIssuer --show-computer-id\n"
        L"\n"
        L"Non-interactive issue:\n"
        L"  SearchClientTokenIssuer --device-type usb --drive E: "
        L"--name \"Ivanov I.I.\" --id C-001 [--defaults path] [--yes]\n"
        L"    [--keystore path] [--password-env TOKEN_ISSUER_PASSWORD]\n"
        L"    [--allow-manual-serial SERIAL] [--issuer ...] [--notes ...]\n"
        L"  SearchClientTokenIssuer --device-type computer "
        L"--name \"Ivanov I.I.\" --id C-001 --output <path>\n"
        L"    [--defaults path] [--yes] [--keystore path] "
        L"[--password-env TOKEN_ISSUER_PASSWORD]\n"
        L"    [--issuer ...] [--notes ...]\n"
        L"\n"
        L"Token string fields must be printable ASCII only (no Cyrillic).\n"
        L"Keystore: RSA-2048; private key encrypted PKCS#8 AES-256-CBC.\n"
        L"Tokens: format_version 1, signature.alg RS256 (server verifies).\n"
        L"\n"
        L"Exit codes: 0 ok, 1 error, 2 cancelled, 3 no eligible volume/serial\n");
}

std::optional<std::wstring> Option(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(
                    Utf8(name) + " requires a value");
            }
            return args[i + 1];
        }
    }
    return std::nullopt;
}

bool HasFlag(const std::vector<std::wstring>& args, const std::wstring& name)
{
    for (const auto& arg : args) {
        if (arg == name) {
            return true;
        }
    }
    return false;
}

fs::path ExeDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return fs::current_path();
    }
    return fs::path(buffer).parent_path();
}

std::string RequireAsciiField(const std::string& label, std::string value)
{
    value = token_issuer::TrimCopy(std::move(value));
    if (value.empty()) {
        throw std::runtime_error(label + " must be non-empty");
    }
    if (!token_issuer::IsAsciiTokenField(value)) {
        throw std::runtime_error(
            label +
            " must be printable ASCII only (A-Z a-z 0-9 space .,-_()/+#@:; "
            "no Cyrillic)");
    }
    return value;
}

std::string PromptAsciiField(
    const std::wstring& prompt,
    const std::string& default_value,
    bool allow_empty_default = false)
{
    for (;;) {
        WriteOut(prompt);
        if (!default_value.empty() || allow_empty_default) {
            WriteOut(L" [");
            WriteOut(Wide(default_value));
            WriteOut(L"]");
        }
        WriteOut(L": ");
        const std::string answer =
            token_issuer::TrimCopy(Utf8(ReadLine()));
        const std::string value =
            answer.empty() ? default_value : answer;
        if (value.empty() && !allow_empty_default) {
            WriteErr(L"Value must be non-empty.\n");
            continue;
        }
        if (!value.empty() && !token_issuer::IsAsciiTokenField(value)) {
            WriteErr(
                L"Use printable ASCII only (no Cyrillic / no quotes).\n");
            continue;
        }
        return value;
    }
}

std::string PromptPath(const std::wstring& prompt)
{
    for (;;) {
        WriteOut(prompt);
        WriteOut(L": ");
        const std::string answer = token_issuer::TrimCopy(Utf8(ReadLine()));
        if (answer.empty()) {
            WriteErr(L"Value must be non-empty.\n");
            continue;
        }
        return answer;
    }
}

bool PromptYesNo(const std::wstring& prompt, bool default_yes)
{
    for (;;) {
        WriteOut(prompt);
        WriteOut(default_yes ? L" [Y/n]: " : L" [y/N]: ");
        const std::wstring answer = ReadLine();
        if (answer.empty()) {
            return default_yes;
        }
        if (answer == L"y" || answer == L"Y" || answer == L"yes" ||
            answer == L"YES")
        {
            return true;
        }
        if (answer == L"n" || answer == L"N" || answer == L"no" ||
            answer == L"NO")
        {
            return false;
        }
        WriteErr(L"Enter Y or N.\n");
    }
}

std::string ResolvePassword(
    const std::vector<std::wstring>& args,
    bool creating)
{
    if (const auto env_name = Option(args, L"--password-env")) {
        const std::string name = Utf8(*env_name);
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) {
            throw std::runtime_error(
                "environment variable not set: " + name);
        }
        std::string password(value);
        free(value);
        if (password.empty()) {
            throw std::runtime_error(
                "environment variable is empty: " + name);
        }
        return password;
    }

    if (creating) {
        for (;;) {
            WriteOut(L"Create keystore password: ");
            const std::wstring a = ReadPasswordLine();
            WriteOut(L"Confirm password: ");
            const std::wstring b = ReadPasswordLine();
            if (a.empty()) {
                WriteErr(L"Password must not be empty.\n");
                continue;
            }
            if (a != b) {
                WriteErr(L"Passwords do not match.\n");
                continue;
            }
            return Utf8(a);
        }
    }

    WriteOut(L"Keystore password: ");
    return Utf8(ReadPasswordLine());
}

std::string EnsureKeystore(
    const token_issuer::KeystorePaths& paths,
    const std::vector<std::wstring>& args)
{
    if (!token_issuer::KeystoreExists(paths)) {
        WriteOut(
            L"No RSA keystore found. Generating RSA-2048 key pair "
            L"(private key encrypted with your password)...\n");
        const std::string password = ResolvePassword(args, true);
        token_issuer::GenerateKeyPair(paths, password);
        WriteOut(L"Keystore created at: ");
        WriteOut(paths.root.wstring());
        WriteOut(L"\n");
        return token_issuer::UnlockPrivateKey(paths, password);
    }

    const std::string password = ResolvePassword(args, false);
    return token_issuer::UnlockPrivateKey(paths, password);
}

int RunInitKeystore(
    const std::vector<std::wstring>& args,
    const token_issuer::KeystorePaths& keystore)
{
    if (token_issuer::KeystoreExists(keystore)) {
        WriteErr(L"Keystore already exists: ");
        WriteErr(keystore.root.wstring());
        WriteErr(L"\nRefuse to overwrite. Delete the folder first.\n");
        return kExitError;
    }
    (void)EnsureKeystore(keystore, args);
    WriteOut(L"public:  ");
    WriteOut(keystore.public_key.wstring());
    WriteOut(L"\nprivate: ");
    WriteOut(keystore.private_enc.wstring());
    WriteOut(L"\n");
    return kExitOk;
}

int RunExportPublic(
    const token_issuer::KeystorePaths& keystore,
    const fs::path& destination_arg)
{
    if (!token_issuer::KeystoreExists(keystore)) {
        throw std::runtime_error(
            "keystore not found; run --init-keystore first");
    }
    fs::path destination = destination_arg;
    if (fs::is_directory(destination)) {
        destination /= "issuer-public.pem";
    }
    token_issuer::ExportPublicKey(keystore, destination);
    WriteOut(L"Exported public key to: ");
    WriteOut(destination.wstring());
    WriteOut(L"\n");
    return kExitOk;
}

TokenFields FieldsFromDefaults(const nlohmann::json& defaults)
{
    TokenFields fields;
    fields.client_id = defaults.value("client_id", std::string());
    fields.client_name = defaults.value("client_name", std::string());
    fields.device_type = defaults.value("device_type", std::string("usb"));
    fields.device_id = defaults.value("device_id", std::string());
    fields.issuer = defaults.value("issuer", std::string("auth-server"));
    fields.notes = defaults.value("notes", std::string());
    fields.issued_at = defaults.value("issued_at", std::string());
    if (fields.issued_at.empty()) {
        fields.issued_at = token_issuer::NowUtcIso8601();
    }
    if (defaults.contains("expires_at")) {
        fields.expires_at = defaults.at("expires_at");
    } else {
        fields.expires_at = nullptr;
    }
    return fields;
}

void PrintRegisterHint(const fs::path& token_path)
{
    WriteOut(L"\nToken written: ");
    WriteOut(token_path.wstring());
    WriteOut(L"\n");
    WriteOut(
        L"Register on the server (separate step):\n"
        L"  AuthDbTool --db <data>\\auth_clients.sqlite add-from-token "
        L"--token \"");
    WriteOut(token_path.wstring());
    WriteOut(
        L"\"\n"
        L"  or scripts\\Register-AuthClientFromToken.ps1\n"
        L"Put issuer-public.pem next to auth_clients.sqlite "
        L"(--export-public <data-dir>).\n");
}

token_issuer::TokenSignature SignFields(
    const TokenFields& fields,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    using token_issuer::TrimCopy;
    const auto device_type = TrimCopy(fields.device_type);
    std::string device_id = TrimCopy(fields.device_id);
    if (device_type == auth::kDeviceTypeUsb) {
        device_id = auth::NormalizeUsbDeviceId(device_id);
    } else if (device_type == auth::kDeviceTypeComputer) {
        auto uuid = auth::NormalizeComputerUuid(device_id);
        if (!uuid) {
            throw std::runtime_error(
                "computer device_id must be a usable SMBIOS UUID");
        }
        device_id = *uuid;
    } else {
        throw std::runtime_error("device_type must be usb or computer");
    }
    const auto message = token_issuer::BuildIdentitySigningMessage(
        TrimCopy(fields.client_id),
        TrimCopy(fields.client_name),
        device_type,
        device_id);
    auto signature =
        token_issuer::SignTokenPayload(message, private_pem);

    std::ifstream pub(keystore.public_key, std::ios::binary);
    if (!pub) {
        throw std::runtime_error(
            "cannot read public key for self-check: " +
            keystore.public_key.string());
    }
    const std::string public_pem(
        (std::istreambuf_iterator<char>(pub)),
        std::istreambuf_iterator<char>());
    if (!token_issuer::VerifyTokenSignature(
            message, signature.value, public_pem))
    {
        throw std::runtime_error("token signature self-check failed");
    }
    return signature;
}

int WriteWithConfirm(
    const fs::path& token_path,
    const TokenFields& fields,
    bool yes,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    const auto signature = SignFields(fields, private_pem, keystore);

    WriteOut(L"\nPreview:\n");
    WriteOut(Wide(token_issuer::PreviewTokenJson(fields, signature)));
    WriteOut(L"\n");

    if (fs::exists(token_path) && !yes) {
        if (!PromptYesNo(L"Token file exists. Overwrite?", false)) {
            return kExitCancelled;
        }
    }

    token_issuer::WriteTokenFile(token_path, fields, signature);
    PrintRegisterHint(token_path);
    return kExitOk;
}

void PromptCommonFields(TokenFields& fields)
{
    std::string default_id = fields.client_id;
    if (default_id.empty()) {
        default_id = token_issuer::GenerateClientId();
    }

    fields.client_name = PromptAsciiField(L"client_name", fields.client_name);
    fields.client_id = PromptAsciiField(L"client_id", default_id);
    fields.issuer = PromptAsciiField(L"issuer", fields.issuer, true);
    fields.notes = PromptAsciiField(L"notes", fields.notes, true);

    WriteOut(L"issued_at default is now UTC; expires_at stays null unless "
             L"set in defaults.\n");
    if (fields.issued_at.empty()) {
        fields.issued_at = token_issuer::NowUtcIso8601();
    }
}

void ApplyOptionalIssuerNotes(
    TokenFields& fields,
    const std::vector<std::wstring>& args)
{
    if (const auto issuer = Option(args, L"--issuer")) {
        fields.issuer = RequireAsciiField("issuer", Utf8(*issuer));
    }
    if (const auto notes = Option(args, L"--notes")) {
        fields.notes = token_issuer::TrimCopy(Utf8(*notes));
        if (!token_issuer::IsAsciiTokenField(fields.notes)) {
            throw std::runtime_error(
                "notes must be printable ASCII only (no Cyrillic)");
        }
    }
}

fs::path ResolveOutputPath(const std::wstring& output)
{
    fs::path path(output);
    if (path.empty()) {
        throw std::runtime_error("--output path must be non-empty");
    }
    if (fs::is_directory(path) ||
        (!path.has_filename() || path.filename() == "." ||
         path.filename() == ".."))
    {
        path /= token_issuer::kTokenFileName;
    }
    return path;
}

int RunInteractiveUsb(
    const std::vector<std::wstring>& args,
    TokenFields fields,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    const auto volumes = token_issuer::ListEligibleRemovableVolumes();
    if (volumes.empty()) {
        WriteErr(
            L"No removable volumes with a readable hardware serial.\n");
        return kExitNoVolume;
    }

    WriteOut(L"\nEligible removable volumes:\n");
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        WriteOut(L"  ");
        WriteOut(std::to_wstring(i + 1));
        WriteOut(L" - ");
        WriteOut(Wide(volumes[i].drive_letter));
        WriteOut(L"  serial=");
        WriteOut(Wide(volumes[i].serial));
        WriteOut(L"\n");
    }
    WriteOut(L"  0 - Cancel\n");

    int selected = -1;
    for (;;) {
        WriteOut(L"Select volume: ");
        const std::wstring answer = ReadLine();
        try {
            selected = std::stoi(Utf8(answer));
        } catch (...) {
            WriteErr(L"Enter a number from the list.\n");
            continue;
        }
        if (selected == 0) {
            return kExitCancelled;
        }
        if (selected >= 1 &&
            static_cast<std::size_t>(selected) <= volumes.size())
        {
            break;
        }
        WriteErr(L"Enter a number from the list.\n");
    }

    const auto& volume = volumes[static_cast<std::size_t>(selected - 1)];
    fields.device_type = std::string(auth::kDeviceTypeUsb);
    fields.device_id = volume.serial;
    PromptCommonFields(fields);

    if (!PromptYesNo(L"Write token to volume root?", true)) {
        return kExitCancelled;
    }

    const fs::path token_path =
        fs::path(Wide(volume.drive_letter + "\\")) /
        token_issuer::kTokenFileName;
    const bool yes = HasFlag(args, L"--yes");
    return WriteWithConfirm(
        token_path, fields, yes, private_pem, keystore);
}

int RunInteractiveComputer(
    const std::vector<std::wstring>& args,
    TokenFields fields,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    const std::string uuid = token_issuer::RequireComputerDeviceId();
    WriteOut(L"Computer device_id (Win32_ComputerSystemProduct.UUID): ");
    WriteOut(Wide(uuid));
    WriteOut(L"\n");

    fields.device_type = std::string(auth::kDeviceTypeComputer);
    fields.device_id = uuid;
    PromptCommonFields(fields);

    std::string output = PromptPath(L"output path");
    const fs::path token_path = ResolveOutputPath(Wide(output));
    if (const auto parent = token_path.parent_path(); !parent.empty()) {
        fs::create_directories(parent);
    }
    const bool yes = HasFlag(args, L"--yes");
    return WriteWithConfirm(
        token_path, fields, yes, private_pem, keystore);
}

int RunInteractive(
    const std::vector<std::wstring>& args,
    const nlohmann::json& defaults,
    const token_issuer::KeystorePaths& keystore)
{
    const std::string private_pem = EnsureKeystore(keystore, args);
    TokenFields fields = FieldsFromDefaults(defaults);

    WriteOut(L"\nToken type:\n  1 - USB\n  2 - Computer\n  0 - Cancel\n");
    for (;;) {
        WriteOut(L"Select token type: ");
        const std::string answer = token_issuer::TrimCopy(Utf8(ReadLine()));
        if (answer == "0") {
            return kExitCancelled;
        }
        if (answer == "1") {
            return RunInteractiveUsb(args, std::move(fields), private_pem, keystore);
        }
        if (answer == "2") {
            return RunInteractiveComputer(
                args, std::move(fields), private_pem, keystore);
        }
        WriteErr(L"Enter 1, 2, or 0.\n");
    }
}

int RunNonInteractiveUsb(
    const std::vector<std::wstring>& args,
    TokenFields fields,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    const auto drive_opt = Option(args, L"--drive");
    if (!drive_opt) {
        throw std::runtime_error("USB token requires --drive");
    }

    if (const auto manual = Option(args, L"--allow-manual-serial")) {
        WriteErr(
            L"WARNING: using manual USB device_id override; "
            L"prefer hardware serial from the volume.\n");
        fields.device_id = RequireAsciiField(
            "device_id",
            auth::NormalizeUsbDeviceId(Utf8(*manual)));
    } else {
        const std::string drive = token_issuer::NormalizeDriveLetter(
            Utf8(*drive_opt));
        if (drive.empty()) {
            throw std::runtime_error("invalid --drive");
        }
        const std::string serial =
            token_issuer::GetSerialForDriveLetter(drive);
        if (serial.empty() || serial == "(UNKNOWN)") {
            WriteErr(L"Cannot read hardware serial for drive ");
            WriteErr(Wide(drive));
            WriteErr(L"\n");
            return kExitNoVolume;
        }
        fields.device_id = auth::NormalizeUsbDeviceId(serial);
    }

    fields.device_type = std::string(auth::kDeviceTypeUsb);
    if (fields.issued_at.empty()) {
        fields.issued_at = token_issuer::NowUtcIso8601();
    }

    fs::path token_path;
    if (const auto output = Option(args, L"--output")) {
        token_path = ResolveOutputPath(*output);
    } else {
        const std::string drive = token_issuer::NormalizeDriveLetter(
            Utf8(*drive_opt));
        token_path = fs::path(Wide(drive + "\\")) / token_issuer::kTokenFileName;
    }

    const bool yes = HasFlag(args, L"--yes");
    if (!yes && fs::exists(token_path)) {
        if (!PromptYesNo(L"Token file exists. Overwrite?", false)) {
            return kExitCancelled;
        }
    } else if (!yes) {
        if (!PromptYesNo(L"Write USB token?", true)) {
            return kExitCancelled;
        }
    }

    return WriteWithConfirm(
        token_path, fields, true, private_pem, keystore);
}

int RunNonInteractiveComputer(
    const std::vector<std::wstring>& args,
    TokenFields fields,
    std::string_view private_pem,
    const token_issuer::KeystorePaths& keystore)
{
    const auto output = Option(args, L"--output");
    if (!output) {
        throw std::runtime_error("computer token requires --output <path>");
    }

    const std::string uuid = token_issuer::RequireComputerDeviceId();
    WriteOut(L"Computer device_id: ");
    WriteOut(Wide(uuid));
    WriteOut(L"\n");

    fields.device_type = std::string(auth::kDeviceTypeComputer);
    fields.device_id = uuid;
    if (fields.issued_at.empty()) {
        fields.issued_at = token_issuer::NowUtcIso8601();
    }

    const fs::path token_path = ResolveOutputPath(*output);
    if (const auto parent = token_path.parent_path(); !parent.empty()) {
        fs::create_directories(parent);
    }
    const bool yes = HasFlag(args, L"--yes");
    if (!yes && fs::exists(token_path)) {
        if (!PromptYesNo(L"Token file exists. Overwrite?", false)) {
            return kExitCancelled;
        }
    } else if (!yes) {
        if (!PromptYesNo(L"Write computer token?", true)) {
            return kExitCancelled;
        }
    }

    return WriteWithConfirm(
        token_path, fields, true, private_pem, keystore);
}

int RunNonInteractive(
    const std::vector<std::wstring>& args,
    const nlohmann::json& defaults,
    const token_issuer::KeystorePaths& keystore)
{
    const auto name_opt = Option(args, L"--name");
    const auto id_opt = Option(args, L"--id");
    if (!name_opt || !id_opt) {
        throw std::runtime_error(
            "non-interactive mode requires --name --id");
    }

    std::string device_type;
    if (const auto type = Option(args, L"--device-type")) {
        device_type = token_issuer::TrimCopy(Utf8(*type));
    } else if (Option(args, L"--drive")) {
        device_type = std::string(auth::kDeviceTypeUsb);
    } else if (Option(args, L"--output")) {
        device_type = std::string(auth::kDeviceTypeComputer);
    } else {
        throw std::runtime_error(
            "non-interactive mode requires --device-type usb|computer");
    }
    if (!auth::IsSupportedDeviceType(device_type)) {
        throw std::runtime_error("device_type must be usb or computer");
    }

    const std::string private_pem = EnsureKeystore(keystore, args);
    TokenFields fields = FieldsFromDefaults(defaults);
    fields.client_name = RequireAsciiField("client_name", Utf8(*name_opt));
    fields.client_id = RequireAsciiField("client_id", Utf8(*id_opt));
    fields.device_type = device_type;
    ApplyOptionalIssuerNotes(fields, args);

    if (device_type == auth::kDeviceTypeUsb) {
        return RunNonInteractiveUsb(args, std::move(fields), private_pem, keystore);
    }
    return RunNonInteractiveComputer(
        args, std::move(fields), private_pem, keystore);
}

int RunShowComputerId()
{
    const std::string uuid = token_issuer::RequireComputerDeviceId();
    WriteOut(Wide(uuid));
    WriteOut(L"\n");
    return kExitOk;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        std::vector<std::wstring> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }

        if (HasFlag(args, L"--help") || HasFlag(args, L"-h")) {
            PrintUsage();
            return kExitOk;
        }

        fs::path defaults_path;
        if (const auto value = Option(args, L"--defaults")) {
            defaults_path = *value;
        }

        fs::path keystore_root;
        if (const auto value = Option(args, L"--keystore")) {
            keystore_root = *value;
        } else {
            const fs::path beside = ExeDirectory() / "keys";
            if (fs::is_directory(beside) ||
                fs::is_regular_file(beside / "private.enc.pem") ||
                fs::is_regular_file(beside / "private.stub.enc"))
            {
                keystore_root = beside;
            } else {
                keystore_root = token_issuer::DefaultKeystoreRoot();
            }
        }

        const auto keystore =
            token_issuer::ResolveKeystorePaths(keystore_root);

        if (HasFlag(args, L"--init-keystore")) {
            return RunInitKeystore(args, keystore);
        }
        if (const auto export_to = Option(args, L"--export-public")) {
            return RunExportPublic(keystore, *export_to);
        }
        if (HasFlag(args, L"--show-computer-id")) {
            return RunShowComputerId();
        }

        const auto defaults = token_issuer::LoadTokenDefaults(
            ExeDirectory(), defaults_path);

        const bool non_interactive =
            Option(args, L"--drive").has_value() ||
            Option(args, L"--name").has_value() ||
            Option(args, L"--id").has_value() ||
            Option(args, L"--device-type").has_value() ||
            Option(args, L"--output").has_value();

        if (non_interactive) {
            return RunNonInteractive(args, defaults, keystore);
        }
        return RunInteractive(args, defaults, keystore);
    } catch (const std::exception& ex) {
        WriteErr(L"SearchClientTokenIssuer error: ");
        WriteErr(Wide(ex.what()));
        WriteErr(L"\n");
        return kExitError;
    }
}
