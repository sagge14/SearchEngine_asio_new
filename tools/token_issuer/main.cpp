#include "CryptoStub.hpp"
#include "TokenAscii.hpp"
#include "TokenDefaults.hpp"
#include "TokenDocument.hpp"
#include "IdentitySigning.hpp"
#include "VolumeSerial.hpp"
#include "ComputerIdentity.hpp"
#include "ComputerTokenPath.hpp"
#include "TokenLoader.hpp"
#include "Auth/DeviceIdentity.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
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

// Console text is UTF-16; redirected output remains UTF-8. BAT stays ASCII.
bool russian_ui = false;
const wchar_t* Ui(const wchar_t* english, const wchar_t* russian)
{
    return russian_ui ? russian : english;
}

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitCancelled = 2;
constexpr int kExitNoVolume = 3;
constexpr int kExitRetrySaveLocation = 4;

enum class OverwriteDeclineAction {
    CancelIssuance,
    RetrySaveLocation,
};

enum class ComputerSaveChoice {
    Cancel = 0,
    Standard = 1,
    Manual = 2,
};

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


std::wstring ErrorText(const std::string& error)
{
    if (!russian_ui) {
        return Wide(error);
    }
    const std::pair<const char*, const wchar_t*> messages[] = {
        {"cannot read interactive input", L"Ввод завершён до окончания операции."},
        {"cannot read from the console", L"Не удалось прочитать ввод из консоли."},
        {"cannot open the save dialog", L"Не удалось открыть окно сохранения."},
        {"cannot open the request selection dialog", L"Не удалось открыть окно выбора заявки."},
        {"local token belongs to another device; it was not changed", L"Локальный токен принадлежит другому устройству. Файл не изменён."},
        {"saved request belongs to another computer; it was not changed", L"Сохранённая заявка принадлежит другому ПК. Файл не изменён."},
        {"request cannot replace the local signed token", L"Заявку нужно сохранить отдельно от подписанного токена."},
        {"--output path must be non-empty", L"Укажите непустой путь сохранения (--output)."},
        {"language must be auto, ru or en", L"Язык должен быть ru, en или auto."},
        {"computer token requires --output <path>", L"Для токена компьютера укажите путь сохранения (--output)."},
        {"non-interactive mode requires --name --id", L"Укажите имя (--name) и идентификатор (--id) клиента."},
        {"device_type must be usb or computer", L"Тип устройства должен быть usb или computer."},
        {"USB token requires --drive", L"Для токена USB укажите накопитель (--drive)."},
        {"invalid --drive", L"Неверно указан накопитель (--drive)."},
        {"signed token must be saved separately from the request", L"Сохраните подписанный токен отдельно от исходной заявки."},
        {"keystore not found; run --init-keystore first", L"Хранилище ключей не найдено. Сначала выполните --init-keystore."},
        {"signing requires an existing issuer keystore; use the authorized signing computer or --keystore. No new key was created", L"Для подписи нужно существующее хранилище ключей. Используйте компьютер с правом подписи или укажите --keystore. Новый ключ не создавался."},
        {"select an unsigned searchclient-auth-request file", L"Выберите неподписанный файл заявки searchclient-auth-request."},
        {"unsupported unsigned request format_version", L"Версия формата заявки не поддерживается."},
        {"unsigned request contains unexpected fields", L"Заявка содержит лишние поля."},
        {"unsigned request must be a JSON object", L"Заявка должна содержать объект JSON."},
        {"unsigned request must be 1..65536 bytes", L"Размер заявки должен быть от 1 до 65536 байт."},
        {"invalid unsigned request JSON", L"Неверный формат JSON заявки."},
        {"cannot open unsigned request file", L"Не удалось открыть файл заявки."},
        {"cannot write unsigned request file", L"Не удалось записать файл заявки."},
        {"failed writing unsigned request file", L"Ошибка записи файла заявки."},
        {"token signature self-check failed", L"Проверка созданной подписи не пройдена."},
    };
    for (const auto& message : messages) {
        if (error == message.first) {
            return message.second;
        }
    }
    for (const auto& field : {"client_name", "client_id", "issuer", "notes", "device_id"}) {
        const std::string prefix(field);
        if (error == prefix + " must be non-empty") {
            return L"Обязательное поле " + Wide(prefix) + L": введите значение.";
        }
        if (error.rfind(prefix + " must be printable ASCII", 0) == 0) {
            return L"Поле " + Wide(prefix) + L": используйте латиницу и допустимые знаки ASCII.";
        }
    }
    if (error.rfind("cannot decrypt private key", 0) == 0) {
        return L"Не удалось открыть закрытый ключ: неверный пароль или повреждённый файл ключа.";
    }
    if (error.rfind("cannot obtain a usable Win32_ComputerSystemProduct.UUID", 0) == 0) {
        return L"Не удалось получить действительный UUID компьютера. Пустые UUID, все нули и все F запрещены.";
    }
    if (error.rfind("environment variable is empty: ", 0) == 0 ||
        error.rfind("environment variable not set: ", 0) == 0) {
        return L"Переменная окружения с паролем отсутствует или пуста.";
    }
    if (error.rfind("invalid request-mode option: ", 0) == 0) {
        return L"Неверный или незаполненный параметр заявки: " + Wide(error.substr(29));
    }
    // Library/OS diagnostics retain their machine text, separated from the UI.
    return L"Техническая диагностика: " + Wide(error);
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
    if (russian_ui) {
        WriteOut(
            L"SearchClientTokenIssuer — выпуск токенов доступа USB и ПК\n"
            L"Без параметров: выбор языка, затем меню из четырёх операций.\n"
            L"Имя и идентификатор обязательны; вводите латиницей.\n"
            L"Издатель и примечание необязательны. Пустой пароль запрещён.\n\n"
            L"Параметры:\n"
            L"  --language ru|en|auto\n"
            L"  --create-request [--name NAME] [--id ID] [--output PATH] [--yes]\n"
            L"  --sign-request FILE [--output PATH] [--keystore PATH]\n"
            L"    [--password-env NAME] [--yes]\n"
            L"  --device-type usb --drive E: --name NAME --id ID\n"
            L"  --device-type computer --name NAME --id ID --output PATH\n"
            L"    [--defaults PATH] [--issuer TEXT] [--notes TEXT] [--yes]\n"
            L"    [--keystore PATH] [--password-env NAME]\n"
            L"  --init-keystore [--keystore PATH] [--password-env NAME]\n"
            L"  --export-public PATH [--keystore PATH]\n"
            L"  --show-computer-id\n\n"
            L"Без --output заявка и подписанный ответ сохраняются через окно выбора файла.\n"
            L"Коды завершения: 0 — успех, 1 — ошибка, 2 — отмена, 3 — нет накопителя.\n");
        return;
    }
    WriteOut(L"Language: --language ru|en|auto (interactive default: Russian).\n");
    WriteErr(
        L"SearchClientTokenIssuer - issue searchclient-auth-token.json\n"
        L"\n"
        L"Interactive (default):\n"
        L"  SearchClientTokenIssuer\n"
        L"    Token type: 1 USB, 2 Computer\n"
        L"    Computer save: 1 current user profile, 2 manual Save dialog\n"
        L"    3 - Create unsigned computer request (no signing key needed)\n"
        L"    4 - Sign a request received from another computer\n"
        L"\n"
        L"  SearchClientTokenIssuer --init-keystore [--keystore path] "
        L"[--password-env NAME]\n"
        L"  SearchClientTokenIssuer --export-public <file-or-dir> "
        L"[--keystore path]\n"
        L"  SearchClientTokenIssuer --show-computer-id\n"
        L"  SearchClientTokenIssuer --create-request [--name NAME] [--id ID]\n"
        L"    [--output <file-or-directory>] [--yes]\n"
        L"  SearchClientTokenIssuer --sign-request <request.json>\n"
        L"    [--output <file-or-directory>] [--keystore path]\n"
        L"    [--password-env NAME] [--yes]\n"
        L"  Without --output, a Save dialog lets you choose the location.\n"
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
            if (i + 1 >= args.size() || args[i + 1].rfind(L"--", 0) == 0) {
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

std::optional<std::wstring> BrowseSaveTokenPath(
    const std::optional<fs::path>& initial_dir,
    bool unsigned_request = false)
{
    wchar_t file[32768]{};
    wcsncpy_s(
        file, Wide(unsigned_request ? token_issuer::kRequestFileName :
                                     token_issuer::kTokenFileName).c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetConsoleWindow();
    ofn.lpstrFilter = unsigned_request ?
        Ui(L"Unsigned computer request (*.json)\0*.json\0"
        L"All files (*.*)\0*.*\0", L"Неподписанная заявка ПК (*.json)\0*.json\0Все файлы (*.*)\0*.*\0") :
        Ui(L"Auth token (searchclient-auth-token.json)\0"
        L"searchclient-auth-token.json\0"
        L"JSON (*.json)\0*.json\0"
        L"All files (*.*)\0*.*\0", L"Токен доступа (searchclient-auth-token.json)\0searchclient-auth-token.json\0JSON (*.json)\0*.json\0Все файлы (*.*)\0*.*\0");
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle = unsigned_request ? Ui(L"Save unsigned computer request", L"Сохранить неподписанную заявку ПК") :
                                       Ui(L"Save searchclient-auth-token.json", L"Сохранить токен searchclient-auth-token.json");
    ofn.lpstrDefExt = L"json";
    ofn.Flags =
        OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    std::wstring initial_dir_storage;
    if (initial_dir && !initial_dir->empty()) {
        initial_dir_storage = initial_dir->wstring();
        ofn.lpstrInitialDir = initial_dir_storage.c_str();
    }

    if (GetSaveFileNameW(&ofn)) {
        return std::wstring(file);
    }
    if (CommDlgExtendedError() != 0) {
        throw std::runtime_error("cannot open the save dialog");
    }
    return std::nullopt;
}

std::optional<ComputerSaveChoice> PromptComputerSaveChoice()
{
    for (;;) {
        const auto standard_path = token_issuer::StandardComputerTokenPath();

        WriteOut(Ui(L"\nWhere should the computer token be saved?\n", L"\nКуда сохранить токен компьютера?\n"));
        WriteOut(Ui(L"  1 - Current user profile\n      ", L"  1 - Папка текущего пользователя\n      "));
        if (standard_path) {
            WriteOut(standard_path->wstring());
        } else {
            WriteOut(Ui(L"(unavailable)", L"(недоступна)"));
        }
        WriteOut(Ui(L"\n  2 - Choose location manually\n  0 - Cancel\n", L"\n  2 - Выбрать место вручную\n  0 - Отмена\n"));
        WriteOut(Ui(L"Select save location: ", L"Выберите место сохранения: "));

        const std::wstring answer = ReadLine();
        if (answer == L"0") {
            return ComputerSaveChoice::Cancel;
        }
        if (answer == L"1") {
            return ComputerSaveChoice::Standard;
        }
        if (answer == L"2") {
            return ComputerSaveChoice::Manual;
        }
        WriteErr(Ui(L"Enter 1, 2, or 0.\n", L"Введите 1, 2 или 0.\n"));
    }
}

std::optional<fs::path> PromptManualComputerTokenPath()
{
    const auto initial_dir = token_issuer::StandardComputerTokenDirectory();
    WriteOut(
        Ui(L"Opening file dialog "
        L"(check behind other windows if it is not visible)...\n", L"Открывается окно выбора файла (если его не видно, проверьте за другими окнами)...\n"));
    const auto picked = BrowseSaveTokenPath(initial_dir);
    if (!picked) {
        return std::nullopt;
    }
    return fs::path(*picked);
}

std::optional<fs::path> ResolveInteractiveComputerTokenPath(
    ComputerSaveChoice choice)
{
    if (choice == ComputerSaveChoice::Manual) {
        return PromptManualComputerTokenPath();
    }

    const auto standard_path = token_issuer::StandardComputerTokenPath();
    if (!standard_path) {
        WriteErr(
            Ui(L"Current user LocalAppData location is unavailable.\n"
            L"Please choose another location manually.\n", L"Папка LocalAppData текущего пользователя недоступна.\nВыберите другую папку вручную.\n"));
        return PromptManualComputerTokenPath();
    }

    const auto directory = token_issuer::StandardComputerTokenDirectory();
    std::string error;
    if (!directory ||
        !token_issuer::EnsureComputerTokenDirectory(*directory, &error))
    {
        WriteErr(Ui(L"Could not save token to:\n", L"Не удалось сохранить токен:\n"));
        WriteErr(standard_path->wstring());
        WriteErr(L"\n");
        if (!error.empty()) {
            WriteErr(ErrorText(error));
            WriteErr(L"\n");
        }
        WriteErr(Ui(L"Please choose another location.\n", L"Выберите другую папку.\n"));
        return PromptManualComputerTokenPath();
    }

    return *standard_path;
}

bool EnsureTokenParentDirectory(const fs::path& token_path, std::string* error)
{
    const auto parent = token_path.parent_path();
    if (parent.empty()) {
        return true;
    }
    return token_issuer::EnsureComputerTokenDirectory(parent, error);
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
        if (allow_empty_default) {
            WriteOut(L" [");
            WriteOut(Wide(default_value));
            WriteOut(L"]");
        }
        WriteOut(L": ");
        const std::string answer =
            token_issuer::TrimCopy(Utf8(ReadLine()));
        const std::string value =
            allow_empty_default && answer.empty() ? default_value : answer;
        if (value.empty() && !allow_empty_default) {
            WriteErr(Ui(L"Value must be non-empty.\n", L"Обязательное поле: введите значение.\n"));
            continue;
        }
        if (!value.empty() && !token_issuer::IsAsciiTokenField(value)) {
            WriteErr(
                Ui(L"Use printable ASCII only (no Cyrillic / no quotes).\n", L"Используйте латинские буквы, цифры и допустимые знаки ASCII (без кириллицы и кавычек).\n"));
            continue;
        }
        return value;
    }
}

bool PromptYesNo(const std::wstring& prompt, bool default_yes)
{
    for (;;) {
        WriteOut(prompt);
        WriteOut(default_yes ? Ui(L" [Y/n]: ", L" [Д/н]: ") : Ui(L" [y/N]: ", L" [д/Н]: "));
        const std::wstring answer = ReadLine();
        if (answer.empty()) {
            return default_yes;
        }
        if (answer == L"y" || answer == L"Y" || answer == L"yes" ||
            answer == L"YES" || (russian_ui &&
            (answer == L"д" || answer == L"Д" || answer == L"да" || answer == L"Да" || answer == L"ДА")))
        {
            return true;
        }
        if (answer == L"n" || answer == L"N" || answer == L"no" ||
            answer == L"NO" || (russian_ui &&
            (answer == L"н" || answer == L"Н" || answer == L"нет" || answer == L"Нет" || answer == L"НЕТ")))
        {
            return false;
        }
        WriteErr(Ui(L"Enter Y or N.\n", L"Введите Д (да) или Н (нет).\n"));
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
        if (token_issuer::TrimCopy(password).empty()) {
            throw std::runtime_error(
                "environment variable is empty: " + name);
        }
        return password;
    }

    if (creating) {
        for (;;) {
            WriteOut(Ui(L"Create keystore password: ", L"Задайте пароль хранилища ключей: "));
            const std::wstring a = ReadPasswordLine();
            WriteOut(Ui(L"Confirm password: ", L"Повторите пароль: "));
            const std::wstring b = ReadPasswordLine();
            if (token_issuer::TrimCopy(Utf8(a)).empty()) {
                WriteErr(Ui(L"Password must not be empty.\n", L"Пароль не должен быть пустым или состоять только из пробелов.\n"));
                continue;
            }
            if (a != b) {
                WriteErr(Ui(L"Passwords do not match.\n", L"Пароли не совпадают.\n"));
                continue;
            }
            return Utf8(a);
        }
    }

    for (;;) {
        WriteOut(Ui(L"Keystore password: ", L"Пароль хранилища ключей: "));
        auto password = Utf8(ReadPasswordLine());
        if (!token_issuer::TrimCopy(password).empty()) {
            return password;
        }
        WriteErr(Ui(L"Password must not be empty.\n",
                    L"Пароль не должен быть пустым или состоять только из пробелов.\n"));
    }
}

std::string EnsureKeystore(
    const token_issuer::KeystorePaths& paths,
    const std::vector<std::wstring>& args)
{
    if (!token_issuer::KeystoreExists(paths)) {
        WriteOut(
            Ui(L"No RSA keystore found. Generating RSA-2048 key pair "
            L"(private key encrypted with your password)...\n", L"Хранилище RSA не найдено. Создаётся пара ключей RSA-2048 (закрытый ключ защищён вашим паролем)...\n"));
        const std::string password = ResolvePassword(args, true);
        token_issuer::GenerateKeyPair(paths, password);
        WriteOut(Ui(L"Keystore created at: ", L"Хранилище ключей создано: "));
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
        WriteErr(Ui(L"Keystore already exists: ", L"Хранилище ключей уже существует: "));
        WriteErr(keystore.root.wstring());
        WriteErr(Ui(L"\nRefuse to overwrite. Delete the folder first.\n", L"\nПерезапись существующего хранилища запрещена.\n"));
        return kExitError;
    }
    (void)EnsureKeystore(keystore, args);
    WriteOut(Ui(L"public:  ", L"Открытый ключ: "));
    WriteOut(keystore.public_key.wstring());
    WriteOut(Ui(L"\nprivate: ", L"\nЗакрытый ключ: "));
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
    WriteOut(Ui(L"Exported public key to: ", L"Открытый ключ экспортирован: "));
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
    WriteOut(Ui(L"\nToken written: ", L"\nТокен сохранён: "));
    WriteOut(token_path.wstring());
    WriteOut(L"\n");
    WriteOut(
        Ui(L"Register on the server (separate step):\n"
        L"  AuthDbTool --db <data>\\auth_clients.sqlite add-from-token "
        L"--token \"", L"Зарегистрируйте токен на сервере (отдельное действие):\n  AuthDbTool --db <data>\\auth_clients.sqlite add-from-token --token \""));
    WriteOut(token_path.wstring());
    WriteOut(
        Ui(L"\"\n"
        L"  or scripts\\Register-AuthClientFromToken.ps1\n"
        L"Put issuer-public.pem next to auth_clients.sqlite "
        L"(--export-public <data-dir>).\n", L"\"\n  или scripts\\Register-AuthClientFromToken.ps1\nПоместите issuer-public.pem рядом с auth_clients.sqlite (--export-public <data-dir>).\n"));
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
    const token_issuer::KeystorePaths& keystore,
    OverwriteDeclineAction on_overwrite_decline =
        OverwriteDeclineAction::CancelIssuance)
{
    const auto signature = SignFields(fields, private_pem, keystore);

    WriteOut(Ui(L"\nPreview:\n", L"\nСодержимое токена:\n"));
    WriteOut(Wide(token_issuer::PreviewTokenJson(fields, signature)));
    WriteOut(L"\n");

    if (fs::exists(token_path) && !yes) {
        if (!PromptYesNo(Ui(L"Token file exists. Overwrite?", L"Файл токена уже существует. Перезаписать?"), false)) {
            return on_overwrite_decline ==
                       OverwriteDeclineAction::RetrySaveLocation
                ? kExitRetrySaveLocation
                : kExitCancelled;
        }
    }

    token_issuer::WriteTokenFile(token_path, fields, signature);
    PrintRegisterHint(token_path);
    return kExitOk;
}

void PromptCommonFields(TokenFields& fields)
{
    fields.client_name = PromptAsciiField(Ui(L"Client name (Latin letters, required)", L"Имя клиента (латиницей, обязательно)"), "");
    fields.client_id = PromptAsciiField(Ui(L"Client ID (required)", L"Идентификатор клиента (обязательно)"), "");
    fields.issuer = PromptAsciiField(Ui(L"Issuer (optional; Enter keeps the value in brackets)", L"Издатель (необязательно; Enter — значение в скобках)"), fields.issuer, true);
    fields.notes = PromptAsciiField(Ui(L"Notes (optional; Enter keeps the value in brackets)", L"Примечание (необязательно; Enter — значение в скобках)"), fields.notes, true);

    WriteOut(Ui(L"issued_at default is now UTC; expires_at stays null unless "
             L"set in defaults.\n", L"Дата выдачи устанавливается автоматически (UTC). Срок действия берётся из настроек; по умолчанию не ограничен.\n"));
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

fs::path ResolveOutputPath(
    const std::wstring& output,
    const char* file_name = token_issuer::kTokenFileName)
{
    fs::path path(output);
    if (token_issuer::TrimCopy(Utf8(output)).empty()) {
        throw std::runtime_error("--output path must be non-empty");
    }
    if (fs::is_directory(path) ||
        (!path.has_filename() || path.filename() == "." ||
         path.filename() == ".."))
    {
        path /= file_name;
    }
    return path;
}

std::optional<fs::path> BrowseOpenRequestPath()
{
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetConsoleWindow();
    ofn.lpstrFilter =
        Ui(L"Unsigned computer request (*.json)\0*.json\0"
        L"All files (*.*)\0*.*\0", L"Неподписанная заявка ПК (*.json)\0*.json\0Все файлы (*.*)\0*.*\0");
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle = Ui(L"Select unsigned computer request", L"Выберите неподписанную заявку ПК");
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    WriteOut(Ui(L"Select the request file in the Open dialog.\n", L"Выберите файл заявки в окне открытия.\n"));
    if (GetOpenFileNameW(&ofn)) {
        return fs::path(file);
    }
    if (CommDlgExtendedError() != 0) {
        throw std::runtime_error("cannot open the request selection dialog");
    }
    return std::nullopt;
}

void PrintRequestIdentity(const TokenFields& fields)
{
    WriteOut(Ui(L"\nClient ID: ", L"\nИдентификатор клиента: "));
    WriteOut(Wide(fields.client_id));
    WriteOut(Ui(L"\nClient name: ", L"\nИмя клиента: "));
    WriteOut(Wide(fields.client_name));
    WriteOut(Ui(L"\nComputer UUID: ", L"\nUUID компьютера: "));
    WriteOut(Wide(fields.device_id));
    WriteOut(L"\n");
}

int RunCreateRequest(const std::vector<std::wstring>& args)
{
    // This path must not unlock or create a keystore, even on first launch.
    TokenFields fields;
    fields.device_type = std::string(auth::kDeviceTypeComputer);
    const auto live_uuid = token_issuer::RequireComputerDeviceId();
    fields.device_id = live_uuid;
    const auto directory = token_issuer::StandardComputerTokenDirectory();
    const auto local_token = token_issuer::StandardComputerTokenPath();
    const auto cached_request = directory ? *directory / token_issuer::kRequestFileName : fs::path{};
    if (local_token && fs::exists(*local_token)) {
        const auto existing = auth_db::loadTokenFields(*local_token);
        if (existing.device_type != auth::kDeviceTypeComputer || existing.device_id != live_uuid)
            throw std::runtime_error("local token belongs to another device; it was not changed");
        fields.client_id = existing.client_id;
        fields.client_name = existing.client_name;
        WriteOut(Ui(L"Using identity from the existing local PC token.\n",
                    L"Имя и данные взяты из существующего локального токена ПК.\n"));
    } else if (!cached_request.empty() && fs::exists(cached_request)) {
        fields = token_issuer::LoadComputerRequestFile(cached_request);
        if (fields.device_id != live_uuid)
            throw std::runtime_error("saved request belongs to another computer; it was not changed");
        WriteOut(Ui(L"Using the saved computer request.\n", L"Используется сохранённая заявка этого ПК.\n"));
    }
    const auto name = Option(args, L"--name");
    if (name) fields.client_name = RequireAsciiField("client_name", Utf8(*name));
    if (fields.client_name.empty())
        fields.client_name = PromptAsciiField(Ui(L"Client name (Latin letters)", L"Имя клиента (латиницей, обязательно)"), "");
    const auto id = Option(args, L"--id");
    if (id) fields.client_id = RequireAsciiField("client_id", Utf8(*id));
    if (fields.client_id.empty() || (!id && fields.client_id == "local-machine")) {
        fields.client_id = "PC-" + live_uuid;
        WriteOut(Ui(L"A unique PC identifier is used for this request. The installed token is unchanged.\n",
                    L"Для заявки используется уникальный идентификатор ПК. Установленный токен не изменён.\n"));
    }
    PrintRequestIdentity(fields);

    const auto output = Option(args, L"--output");
    const bool yes = HasFlag(args, L"--yes");
    for (;;) {
        fs::path path;
        if (output) {
            path = ResolveOutputPath(*output, token_issuer::kRequestFileName);
        } else {
            WriteOut(Ui(L"Choose a folder and file name in the Save dialog.\n", L"Выберите папку и имя файла в окне сохранения.\n"));
            const auto picked = BrowseSaveTokenPath(fs::current_path(), true);
            if (!picked) {
                return kExitCancelled;
            }
            path = *picked;
        }
        try {
            if (fs::exists(path) && !yes &&
                !PromptYesNo(Ui(L"Request file exists. Overwrite?", L"Файл заявки уже существует. Перезаписать?"), false))
            {
                if (output) {
                    return kExitCancelled;
                }
                continue;
            }
            std::string error;
            if (!EnsureTokenParentDirectory(path, &error)) {
                throw std::runtime_error(error);
            }
            if (local_token && fs::exists(*local_token) && fs::exists(path) && fs::equivalent(path, *local_token))
                throw std::runtime_error("request cannot replace the local signed token");
            token_issuer::WriteComputerRequestFile(path, fields);
            if (directory && !cached_request.empty()) {
                fs::create_directories(*directory);
                if (!fs::equivalent(path.parent_path(), cached_request.parent_path()) || path.filename() != cached_request.filename())
                    token_issuer::WriteComputerRequestFile(cached_request, fields);
            }
            WriteOut(Ui(L"\nUnsigned request saved: ", L"\nНеподписанная заявка сохранена: "));
            WriteOut(path.wstring());
            WriteOut(Ui(L"\nTake this file to the administrator's signing computer.\n"
                     L"It cannot be used to log in until signed.\n", L"\nПеренесите этот файл на компьютер администратора с правом подписи.\nДо подписания заявку нельзя использовать для подключения.\n"));
            return kExitOk;
        } catch (const std::exception& ex) {
            if (output) {
                throw;
            }
            WriteErr(ErrorText(ex.what()));
            WriteErr(Ui(L"\nPlease choose another save location.\n", L"\nВыберите другое место сохранения.\n"));
        }
    }
}

int RunSignRequest(
    const std::vector<std::wstring>& args,
    const token_issuer::KeystorePaths& keystore)
{
    std::optional<fs::path> request_path;
    if (const auto input = Option(args, L"--sign-request")) {
        request_path = fs::path(*input);
    } else {
        request_path = BrowseOpenRequestPath();
    }
    if (!request_path) {
        return kExitCancelled;
    }
    auto fields = token_issuer::LoadComputerRequestFile(*request_path);
    fields.issuer = "auth-server";
    // Keep the identity from the request; never read the signing PC's UUID.
    PrintRequestIdentity(fields);
    if (!token_issuer::KeystoreExists(keystore)) {
        throw std::runtime_error(
            "signing requires an existing issuer keystore; use the authorized "
            "signing computer or --keystore. No new key was created");
    }
    const bool yes = HasFlag(args, L"--yes");
    if (!yes && !PromptYesNo(Ui(L"Sign this computer request?", L"Подписать заявку этого компьютера?"), false)) {
        return kExitCancelled;
    }

    const auto output = Option(args, L"--output");
    std::optional<token_issuer::TokenSignature> signature;
    for (;;) {
        fs::path path;
        if (output) {
            path = ResolveOutputPath(*output);
        } else {
            WriteOut(Ui(L"Choose where to save the signed token.\n", L"Выберите место для сохранения подписанного токена.\n"));
            const auto picked = BrowseSaveTokenPath(request_path->parent_path());
            if (!picked) {
                return kExitCancelled;
            }
            path = *picked;
        }
        try {
            if (fs::exists(path)) {
                if (fs::equivalent(*request_path, path)) {
                    throw std::runtime_error(
                        "signed token must be saved separately from the request");
                }
                if (!yes && !PromptYesNo(Ui(L"Token file exists. Overwrite?", L"Файл токена уже существует. Перезаписать?"), false)) {
                    if (output) {
                        return kExitCancelled;
                    }
                    continue;
                }
            }
            std::string error;
            if (!EnsureTokenParentDirectory(path, &error)) {
                throw std::runtime_error(error);
            }
            if (!signature) {
                const auto password = ResolvePassword(args, false);
                const auto private_pem = token_issuer::UnlockPrivateKey(keystore, password);
                signature = SignFields(fields, private_pem, keystore);
            }
            token_issuer::WriteTokenFile(path, fields, *signature);
            PrintRegisterHint(path);
            WriteOut(
                Ui(L"Copy the signed token to the requesting computer:\n"
                L"  %LOCALAPPDATA%\\SearchEngine\\searchclient-auth-token.json\n", L"Скопируйте подписанный токен на компьютер, создавший заявку:\n  %LOCALAPPDATA%\\SearchEngine\\searchclient-auth-token.json\n"));
            return kExitOk;
        } catch (const std::exception& ex) {
            if (output) {
                throw;
            }
            WriteErr(ErrorText(ex.what()));
            WriteErr(Ui(L"\nChoose another location to retry, or cancel.\n", L"\nДля повторной попытки выберите другое место или отмените операцию.\n"));
        }
    }
}

void ValidateRequestArguments(const std::vector<std::wstring>& args, bool creating)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == L"--yes" || (creating && arg == L"--create-request")) {
            continue;
        }
        const bool value_option = arg == L"--output" ||
            (creating && (arg == L"--name" || arg == L"--id")) ||
            (!creating && (arg == L"--sign-request" || arg == L"--keystore" ||
                           arg == L"--password-env"));
        if (!value_option || i + 1 >= args.size() || args[i + 1].empty() ||
            args[i + 1].rfind(L"--", 0) == 0)
        {
            throw std::runtime_error("invalid request-mode option: " + Utf8(arg));
        }
        ++i;
    }
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
            Ui(L"No removable volumes with a readable hardware serial.\n", L"Нет съёмных накопителей с доступным аппаратным серийным номером.\n"));
        return kExitNoVolume;
    }

    WriteOut(Ui(L"\nEligible removable volumes:\n", L"\nДоступные съёмные накопители:\n"));
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        WriteOut(L"  ");
        WriteOut(std::to_wstring(i + 1));
        WriteOut(L" - ");
        WriteOut(Wide(volumes[i].drive_letter));
        WriteOut(Ui(L"  serial=", L"  серийный номер="));
        WriteOut(Wide(volumes[i].serial));
        WriteOut(L"\n");
    }
    WriteOut(Ui(L"  0 - Cancel\n", L"  0 - Отмена\n"));

    int selected = -1;
    for (;;) {
        WriteOut(Ui(L"Select volume: ", L"Выберите накопитель: "));
        const std::wstring answer = ReadLine();
        try {
            const auto value = token_issuer::TrimCopy(Utf8(answer));
            std::size_t consumed = 0;
            selected = std::stoi(value, &consumed);
            if (consumed != value.size()) {
                throw std::invalid_argument("volume selection");
            }
        } catch (...) {
            WriteErr(Ui(L"Enter a number from the list.\n", L"Введите номер из списка.\n"));
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
        WriteErr(Ui(L"Enter a number from the list.\n", L"Введите номер из списка.\n"));
    }

    const auto& volume = volumes[static_cast<std::size_t>(selected - 1)];
    fields.device_type = std::string(auth::kDeviceTypeUsb);
    fields.device_id = volume.serial;
    PromptCommonFields(fields);

    if (!PromptYesNo(Ui(L"Write token to volume root?", L"Записать токен в корень накопителя?"), true)) {
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
    WriteOut(Ui(L"Computer device_id (Win32_ComputerSystemProduct.UUID): ", L"Идентификатор компьютера (Win32_ComputerSystemProduct.UUID): "));
    WriteOut(Wide(uuid));
    WriteOut(L"\n");

    fields.device_type = std::string(auth::kDeviceTypeComputer);
    fields.device_id = uuid;
    PromptCommonFields(fields);

    const bool yes = HasFlag(args, L"--yes");

    for (;;) {
        const auto choice = PromptComputerSaveChoice();
        if (!choice || *choice == ComputerSaveChoice::Cancel) {
            return kExitCancelled;
        }

        auto token_path_opt = ResolveInteractiveComputerTokenPath(*choice);
        if (!token_path_opt) {
            return kExitCancelled;
        }

        fs::path token_path = *token_path_opt;
        if (*choice == ComputerSaveChoice::Manual) {
            std::string parent_error;
            if (!EnsureTokenParentDirectory(token_path, &parent_error)) {
                WriteErr(Ui(L"Could not create directory for:\n", L"Не удалось создать папку для:\n"));
                WriteErr(token_path.wstring());
                WriteErr(L"\n");
                if (!parent_error.empty()) {
                    WriteErr(ErrorText(parent_error));
                    WriteErr(L"\n");
                }
                WriteErr(Ui(L"Please choose another location.\n", L"Выберите другую папку.\n"));
                token_path_opt = PromptManualComputerTokenPath();
                if (!token_path_opt) {
                    return kExitCancelled;
                }
                token_path = *token_path_opt;
            }
        }

        for (;;) {
            try {
                const int result = WriteWithConfirm(
                    token_path,
                    fields,
                    yes,
                    private_pem,
                    keystore,
                    OverwriteDeclineAction::RetrySaveLocation);
                if (result == kExitRetrySaveLocation) {
                    WriteOut(Ui(L"Choose a different save location.\n", L"Выберите другое место сохранения.\n"));
                    break;
                }
                return result;
            } catch (const std::exception& ex) {
                WriteErr(Ui(L"Could not save token to:\n", L"Не удалось сохранить токен:\n"));
                WriteErr(token_path.wstring());
                WriteErr(L"\n");
                WriteErr(ErrorText(ex.what()));
                WriteErr(Ui(L"\nPlease choose another location.\n", L"\nВыберите другую папку.\n"));
                token_path_opt = PromptManualComputerTokenPath();
                if (!token_path_opt) {
                    return kExitCancelled;
                }
                token_path = *token_path_opt;
            }
        }
    }
}

int RunInteractive(
    const std::vector<std::wstring>& args,
    const nlohmann::json& defaults,
    const token_issuer::KeystorePaths& keystore)
{
    TokenFields fields = FieldsFromDefaults(defaults);

    WriteOut(Ui(L"\nToken operation:\n"
             L"  1 - Issue signed USB token\n"
             L"  2 - Issue signed token for this computer\n"
             L"  3 - Create unsigned token for this computer (request)\n"
             L"  4 - Sign a received computer request\n"
             L"  0 - Cancel\n", L"\nОперации с токенами:\n  1 - Выпустить подписанный токен USB\n  2 - Выпустить подписанный токен этого компьютера\n  3 - Создать неподписанную заявку этого компьютера\n  4 - Подписать полученную заявку компьютера\n  0 - Отмена\n"));
    for (;;) {
        WriteOut(Ui(L"Select operation: ", L"Выберите операцию: "));
        const std::string answer = token_issuer::TrimCopy(Utf8(ReadLine()));
        if (answer == "0") {
            return kExitCancelled;
        }
        if (answer == "1") {
            const auto private_pem = EnsureKeystore(keystore, args);
            return RunInteractiveUsb(args, std::move(fields), private_pem, keystore);
        }
        if (answer == "2") {
            const auto private_pem = EnsureKeystore(keystore, args);
            return RunInteractiveComputer(
                args, std::move(fields), private_pem, keystore);
        }
        if (answer == "3") {
            return RunCreateRequest(args);
        }
        if (answer == "4") {
            return RunSignRequest(args, keystore);
        }
        WriteErr(Ui(L"Enter 1, 2, 3, 4, or 0.\n", L"Введите 1, 2, 3, 4 или 0.\n"));
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
            Ui(L"WARNING: using manual USB device_id override; "
            L"prefer hardware serial from the volume.\n", L"ВНИМАНИЕ: серийный номер USB указан вручную; рекомендуется использовать аппаратный номер накопителя.\n"));
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
            WriteErr(Ui(L"Cannot read hardware serial for drive ", L"Не удалось прочитать аппаратный серийный номер накопителя "));
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
        if (!PromptYesNo(Ui(L"Token file exists. Overwrite?", L"Файл токена уже существует. Перезаписать?"), false)) {
            return kExitCancelled;
        }
    } else if (!yes) {
        if (!PromptYesNo(Ui(L"Write USB token?", L"Записать токен USB?"), true)) {
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
    WriteOut(Ui(L"Computer device_id: ", L"Идентификатор компьютера: "));
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
        if (!PromptYesNo(Ui(L"Token file exists. Overwrite?", L"Файл токена уже существует. Перезаписать?"), false)) {
            return kExitCancelled;
        }
    } else if (!yes) {
        if (!PromptYesNo(Ui(L"Write computer token?", L"Записать токен компьютера?"), true)) {
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

    TokenFields fields = FieldsFromDefaults(defaults);
    fields.client_name = RequireAsciiField("client_name", Utf8(*name_opt));
    fields.client_id = RequireAsciiField("client_id", Utf8(*id_opt));
    fields.device_type = device_type;
    ApplyOptionalIssuerNotes(fields, args);
    // Reject missing/blank destinations before unlocking or creating keys.
    if (device_type == auth::kDeviceTypeComputer) {
        const auto output = Option(args, L"--output");
        if (!output) {
            throw std::runtime_error("computer token requires --output <path>");
        }
        (void)ResolveOutputPath(*output);
    }
    const std::string private_pem = EnsureKeystore(keystore, args);

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

int Run(const std::vector<std::wstring>& args)
{
    try {

        if (HasFlag(args, L"--help") || HasFlag(args, L"-h")) {
            PrintUsage();
            return kExitOk;
        }

        const bool create_request = HasFlag(args, L"--create-request");
        const bool sign_request = HasFlag(args, L"--sign-request");
        if (create_request || sign_request) {
            ValidateRequestArguments(args, create_request);
        }
        if (create_request) {
            return RunCreateRequest(args);
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

        if (sign_request) {
            return RunSignRequest(args, keystore);
        }

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
        WriteErr(Ui(L"SearchClientTokenIssuer error: ", L"Ошибка утилиты подписи токенов: "));
        WriteErr(ErrorText(ex.what()));
        WriteErr(L"\n");
        return kExitError;
    }
}


void SelectLanguage(const std::optional<std::wstring>& language, bool interactive)
{
    if (language && *language != L"auto") {
        if (*language != L"ru" && *language != L"en") {
            throw std::runtime_error("language must be auto, ru or en");
        }
        russian_ui = *language == L"ru";
        return;
    }
    if (!interactive && !language) {
        return; // Preserve machine CLI behavior; no extra stdin consumed.
    }
    for (;;) {
        WriteOut(L"\nВыберите язык / Select language:\n"
                 L"  1 - Русский (по умолчанию)\n"
                 L"  2 - English\n"
                 L"Ваш выбор / Select [1]: ");
        const auto answer = token_issuer::TrimCopy(Utf8(ReadLine()));
        if (answer.empty() || answer == "1") {
            russian_ui = true;
            return;
        }
        if (answer == "2") {
            russian_ui = false;
            return;
        }
        WriteErr(L"Введите 1 или 2. / Enter 1 or 2.\n");
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    // ReadConsoleW/WriteConsoleW need no process-global code page change.
    bool wrapper = false;
    bool quiet = false;
    int result = kExitError;
    try {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        wrapper = HasFlag(args, L"--console-wrapper");
        quiet = HasFlag(args, L"/quiet");
        args.erase(std::remove(args.begin(), args.end(), L"--console-wrapper"), args.end());
        if (wrapper) {
            args.erase(std::remove(args.begin(), args.end(), L"/quiet"), args.end());
        }
        const auto language = Option(args, L"--language");
        // Strip UI options before strict request-mode argument validation.
        if (language) {
            const auto it = std::find(args.begin(), args.end(), L"--language");
            args.erase(it, it + 2);
        }
        const bool command = HasFlag(args, L"--help") || HasFlag(args, L"-h") ||
            HasFlag(args, L"--create-request") || HasFlag(args, L"--sign-request") ||
            HasFlag(args, L"--init-keystore") || HasFlag(args, L"--export-public") ||
            HasFlag(args, L"--show-computer-id") || HasFlag(args, L"--drive") ||
            HasFlag(args, L"--name") || HasFlag(args, L"--id") ||
            HasFlag(args, L"--device-type") || HasFlag(args, L"--output");
        SelectLanguage(language, (!command || wrapper) && !quiet);
        result = Run(args);
    } catch (const std::exception& ex) {
        WriteErr(Ui(L"Token issuer error: ", L"Ошибка утилиты: "));
        WriteErr(ErrorText(ex.what()));
        WriteErr(L"\n");
    }
    if (wrapper) {
        WriteOut(result == kExitOk ? Ui(L"\nOperation completed.\n", L"\nОперация завершена.\n") :
            result == kExitCancelled ? Ui(L"\nOperation cancelled.\n", L"\nОперация отменена.\n") :
            Ui(L"\nOperation failed.\n", L"\nОперация завершилась ошибкой.\n"));
        DWORD mode = 0;
        if (!quiet && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
            WriteOut(Ui(L"Press Enter to close...", L"Нажмите Enter для закрытия..."));
            try { (void)ReadLine(); } catch (...) {}
        }
    }
    return result;
}
