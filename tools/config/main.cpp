#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "Index/Batch/FullIndexStrategy.h"
#include "Index/DocumentCatalogStorage.h"
#include "MyUtils/WindowsPath.h"
#include "RuntimeDataTransaction.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using windows_path::isAbsoluteWindowsLocalPath;
using windows_path::isAbsoluteWindowsFilesystemPath;

constexpr int kMinYear = 2000;
constexpr int kMaxYear = 2099;
constexpr int kMinFileTimeout = 10;
constexpr int kMaxFileTimeout = 600;
constexpr int kRecommendedFileTimeout = 120;
constexpr int kFormatJsonIndent = 4;
constexpr std::uint64_t kPingCommand = 18;

enum class JsonLineEnding {
    Lf,
    CrLf
};

enum class UiLanguage {
    Russian,
    English
};

struct SystemInfo {
    int logicalProcessors{};
    int recommendedThreads{};
    int maximumThreads{};
    int recommendedReaders{};
    int recommendedSqliteThreads{};
    int currentYear{};
};

int parseInt(const std::wstring& text, const char* field);
std::optional<std::wstring> option(
    const std::vector<std::wstring>& args,
    const std::wstring& name);
std::wstring requiredOption(
    const std::vector<std::wstring>& args,
    const std::wstring& name);

std::string utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr
    );
    if (size <= 0) {
        throw std::runtime_error("cannot encode console text as UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr
    );
    return result;
}

std::wstring utf16(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("cannot decode UTF-8 path text");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

void writeInteractive(const std::wstring& text)
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && output != nullptr &&
        GetConsoleMode(output, &mode))
    {
        DWORD written = 0;
        if (!WriteConsoleW(
                output, text.data(), static_cast<DWORD>(text.size()),
                &written, nullptr))
        {
            throw std::runtime_error("cannot write to the console");
        }
        return;
    }
    std::cout << utf8(text);
    std::cout.flush();
}

std::wstring readInteractiveLine()
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && input != nullptr &&
        GetConsoleMode(input, &mode))
    {
        wchar_t buffer[256]{};
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, 255, &read, nullptr)) {
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
    return std::wstring(line.begin(), line.end());
}

std::wstring trim(std::wstring value)
{
    const auto isSpace = [](wchar_t character) { return iswspace(character); };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

UiLanguage chooseLanguage()
{
    for (;;) {
        writeInteractive(
            L"\nВыберите язык / Select language:\n"
            L"  1 - Русский (по умолчанию)\n"
            L"  2 - English\n"
            L"Ваш выбор / Select [1]: "
        );
        const std::wstring answer = trim(readInteractiveLine());
        if (answer.empty() || answer == L"1") {
            return UiLanguage::Russian;
        }
        if (answer == L"2") {
            return UiLanguage::English;
        }
        writeInteractive(L"Введите 1 или 2. / Enter 1 or 2.\n");
    }
}

UiLanguage interactiveLanguage(const std::vector<std::wstring>& args)
{
    if (const auto value = option(args, L"--language")) {
        if (*value == L"ru") {
            return UiLanguage::Russian;
        }
        if (*value == L"en") {
            return UiLanguage::English;
        }
        if (*value != L"auto") {
            throw std::runtime_error("language must be auto, ru or en");
        }
    }
    return chooseLanguage();
}

bool isValidInstanceId(const std::wstring& value)
{
    if (value.empty() || value.size() > 32) {
        return false;
    }
    const auto isAsciiLetterOrDigit = [](wchar_t character) {
        return (character >= L'A' && character <= L'Z') ||
            (character >= L'a' && character <= L'z') ||
            (character >= L'0' && character <= L'9');
    };
    if (!isAsciiLetterOrDigit(value.front())) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [&](wchar_t character) {
        return isAsciiLetterOrDigit(character) ||
            character == L'-' || character == L'_';
    });
}

int chooseInstanceCommand(const std::vector<std::wstring>& args)
{
    const fs::path outputPath = requiredOption(args, L"--output");
    std::wstring defaultInstance = L"default";
    if (const auto value = option(args, L"--default")) {
        defaultInstance = *value;
    }
    if (!isValidInstanceId(defaultInstance)) {
        defaultInstance = L"default";
    }

    const UiLanguage language = chooseLanguage();
    writeInteractive(language == UiLanguage::Russian
        ? L"\nИдентификатор экземпляра поискового сервера\n"
          L"задаёт отдельное имя службы, каталог данных и индекс.\n\n"
          L"Используйте разные имена, если на компьютере нужны серверы\n"
          L"для разных годов (например year2025 и year2026) или серверы\n"
          L"индексируют разные наборы папок.\n"
          L"Оставьте default, если устанавливается только один сервер.\n"
        : L"\nThe search-server instance id selects a separate Windows service,\n"
          L"data directory and index.\n\n"
          L"Use different ids for different years (for example year2025\n"
          L"and year2026) or when servers index different folder sets.\n"
          L"Keep default when only one server is installed.\n");

    std::wstring instance;
    for (;;) {
        writeInteractive(
            (language == UiLanguage::Russian
                ? L"\nВведите идентификатор [" : L"\nEnter instance id [") +
            defaultInstance + L"]: "
        );
        instance = trim(readInteractiveLine());
        if (instance.empty()) {
            instance = defaultInstance;
        }
        if (isValidInstanceId(instance)) {
            break;
        }
        writeInteractive(language == UiLanguage::Russian
            ? L"Недопустимое имя. Используйте 1-32 латинских букв, цифр, "
              L"дефисов или подчёркиваний; первый символ — буква или цифра.\n"
            : L"Invalid id. Use 1-32 ASCII letters, digits, hyphens or "
              L"underscores; the first character must be a letter or digit.\n");
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write instance selection");
    }
    output << "instance=" << utf8(instance) << "\r\n"
           << "language="
           << (language == UiLanguage::Russian ? "ru" : "en") << "\r\n";
    output.close();
    if (!output) {
        throw std::runtime_error("cannot finish instance selection");
    }

    const std::wstring serviceName = instance == L"default"
        ? L"SearchEngineService" : L"SearchEngineService-" + instance;
    writeInteractive(
        (language == UiLanguage::Russian
            ? L"Выбрана служба: " : L"Selected service: ") +
        serviceName + L"\n"
    );
    return 0;
}

class ScManager final {
public:
    ScManager()
        : handle_(OpenSCManagerW(
            nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE))
    {
        if (handle_ == nullptr) {
            throw std::runtime_error("cannot open Windows Service Control Manager");
        }
    }

    ~ScManager() { CloseServiceHandle(handle_); }

    ScManager(const ScManager&) = delete;
    ScManager& operator=(const ScManager&) = delete;

    SC_HANDLE get() const { return handle_; }

private:
    SC_HANDLE handle_{};
};

struct InstalledSearchEngineService {
    std::wstring instance;
    std::wstring serviceName;
    std::wstring displayName;
};

std::vector<InstalledSearchEngineService> installedSearchEngineInstances()
{
    ScManager manager;
    DWORD bytesNeeded = 0;
    DWORD serviceCount = 0;
    DWORD resumeHandle = 0;
    EnumServicesStatusExW(
        manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
        SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded, &serviceCount,
        &resumeHandle, nullptr
    );
    if (GetLastError() != ERROR_MORE_DATA) {
        if (bytesNeeded == 0) {
            return {};
        }
        throw std::runtime_error("cannot enumerate Windows services");
    }

    std::vector<unsigned char> buffer(bytesNeeded);
    resumeHandle = 0;
    if (!EnumServicesStatusExW(
            manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
            SERVICE_STATE_ALL, buffer.data(),
            static_cast<DWORD>(buffer.size()), &bytesNeeded, &serviceCount,
            &resumeHandle, nullptr))
    {
        throw std::runtime_error("cannot enumerate Windows services");
    }

    constexpr wchar_t prefix[] = L"SearchEngineService-";
    constexpr std::size_t prefixLength =
        sizeof(prefix) / sizeof(prefix[0]) - 1;
    std::vector<InstalledSearchEngineService> instances;
    const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(
        buffer.data());
    for (DWORD index = 0; index < serviceCount; ++index) {
        const std::wstring name = services[index].lpServiceName;
        if (name == L"SearchEngineService") {
            instances.push_back({
                L"default", name, services[index].lpDisplayName
            });
            continue;
        }
        if (name.rfind(prefix, 0) == 0) {
            std::wstring instance = name.substr(prefixLength);
            if (isValidInstanceId(instance)) {
                instances.push_back({
                    std::move(instance), name, services[index].lpDisplayName
                });
            }
        }
    }
    std::sort(
        instances.begin(), instances.end(),
        [](const auto& left, const auto& right) {
            return left.instance < right.instance;
        }
    );
    return instances;
}

// Parse --data-dir <value> from the SCM ImagePath (binary path name).
// Supports both quoted and unquoted argument values.
static std::optional<std::wstring> parseDataDirFromImagePath(
    const std::wstring& imagePath)
{
    // Walk tokens after the executable; find "--data-dir" then grab next token.
    // Simple tokeniser: respect double-quoted tokens.
    std::vector<std::wstring> tokens;
    std::size_t pos = 0;
    const std::size_t len = imagePath.size();
    // Skip the executable token first (may be quoted).
    bool inExe = true;
    while (pos < len) {
        while (pos < len && imagePath[pos] == L' ') ++pos;
        if (pos >= len) break;
        std::wstring tok;
        if (imagePath[pos] == L'"') {
            ++pos;
            while (pos < len && imagePath[pos] != L'"') tok += imagePath[pos++];
            if (pos < len) ++pos; // closing quote
        } else {
            while (pos < len && imagePath[pos] != L' ') tok += imagePath[pos++];
        }
        if (inExe) { inExe = false; continue; } // skip exe path
        tokens.push_back(tok);
    }
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == L"--data-dir") {
            return tokens[i + 1];
        }
    }
    return std::nullopt;
}

int inspectInstalledCommand(const std::vector<std::wstring>& args)
{
    const std::wstring instanceId = option(args, L"--instance").value_or(L"default");
    if (!isValidInstanceId(instanceId)) {
        std::cerr << "ERROR: invalid instance id '" << utf8(instanceId) << "'\n";
        return 1;
    }
    const std::wstring serviceName =
        (instanceId == L"default")
            ? L"SearchEngineService"
            : (L"SearchEngineService-" + instanceId);

    ScManager manager;
    SC_HANDLE hSvc = OpenServiceW(
        manager.get(), serviceName.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        throw std::runtime_error(
            "service not found or insufficient permissions: " +
            utf8(serviceName));
    }
    struct SvcGuard { SC_HANDLE h; ~SvcGuard(){ CloseServiceHandle(h); } } guard{hSvc};

    DWORD bytesNeeded = 0;
    QueryServiceConfigW(hSvc, nullptr, 0, &bytesNeeded);
    std::vector<unsigned char> cfgBuf(bytesNeeded);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
    if (!QueryServiceConfigW(hSvc, cfg, bytesNeeded, &bytesNeeded)) {
        throw std::runtime_error("QueryServiceConfig failed");
    }

    const std::wstring imagePath = cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"";

    // Extract the exe path (first token, possibly quoted).
    std::wstring exePath;
    {
        std::size_t p = 0;
        if (!imagePath.empty() && imagePath[p] == L'"') {
            ++p;
            while (p < imagePath.size() && imagePath[p] != L'"') exePath += imagePath[p++];
        } else {
            while (p < imagePath.size() && imagePath[p] != L' ') exePath += imagePath[p++];
        }
    }

    auto dataDir = parseDataDirFromImagePath(imagePath);
    if (!dataDir) {
        throw std::runtime_error(
            "could not find --data-dir argument in service ImagePath.\n"
            "ImagePath: " + utf8(imagePath));
    }

    // If the data-dir is relative, resolve it relative to the exe directory,
    // matching SearchEngine runtime semantics.
    if (!dataDir->empty() && (*dataDir)[0] != L'\\' &&
        !(dataDir->size() >= 2 && (*dataDir)[1] == L':'))
    {
        std::wstring exeDir = exePath;
        const auto lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos)
            exeDir.resize(lastSlash);
        *dataDir = exeDir + L"\\" + *dataDir;
    }

    const std::wstring settingsPath = *dataDir + L"\\Settings.json";
    const std::wstring endpointPath = *dataDir + L"\\client-endpoint.txt";

    const auto printLine = [](const std::wstring& line) {
        std::cout << utf8(line) << '\n';
    };

    printLine(L"instance=" + instanceId);
    printLine(L"service_name=" + serviceName);
    printLine(L"data_dir=" + *dataDir);
    printLine(L"settings_path=" + settingsPath);
    printLine(L"endpoint_path=" + endpointPath);
    printLine(L"installed_program_path=" + exePath);
    return 0;
}

int chooseInstalledInstanceCommand(const std::vector<std::wstring>& args)
{
    const fs::path outputPath = requiredOption(args, L"--output");
    // Shared picker for uninstall / auth-register (and future callers).
    // Default remains uninstall so older Uninstall-*.bat keep working.
    const std::wstring purpose = [&]() {
        std::wstring value = option(args, L"--purpose").value_or(L"uninstall");
        std::transform(value.begin(), value.end(), value.begin(), towlower);
        return value;
    }();
    if (purpose != L"uninstall" && purpose != L"register-auth" &&
        purpose != L"configure") {
        throw std::runtime_error(
            "choose-installed-instance --purpose must be uninstall, register-auth, or configure");
    }

    const UiLanguage language = chooseLanguage();
    const std::vector<InstalledSearchEngineService> instances =
        installedSearchEngineInstances();
    if (instances.empty()) {
        writeInteractive(language == UiLanguage::Russian
            ? L"\nУстановленные службы SearchEngine не найдены.\n"
            : L"\nNo installed SearchEngine services were found.\n");
        return 3;
    }

    const wchar_t* promptRu =
        purpose == L"register-auth"
            ? L"\nВыберите службу SearchEngine для регистрации auth-клиента:\n"
        : purpose == L"configure"
            ? L"\nВыберите службу SearchEngine для изменения конфигурации:\n"
            : L"\nВыберите службу SearchEngine для полного удаления:\n";
    const wchar_t* promptEn =
        purpose == L"register-auth"
            ? L"\nSelect the SearchEngine service to register the auth client:\n"
        : purpose == L"configure"
            ? L"\nSelect the SearchEngine service to configure:\n"
            : L"\nSelect the SearchEngine service to remove completely:\n";
    writeInteractive(language == UiLanguage::Russian ? promptRu : promptEn);
    for (std::size_t index = 0; index < instances.size(); ++index) {
        writeInteractive(
            L"  " + std::to_wstring(index + 1) + L" - " +
            instances[index].serviceName + L"\n"
        );
        if (!instances[index].displayName.empty() &&
            instances[index].displayName != instances[index].serviceName)
        {
            writeInteractive(
                (language == UiLanguage::Russian
                    ? L"      В диспетчере служб: "
                    : L"      Services display name: ") +
                instances[index].displayName + L"\n"
            );
        }
    }
    writeInteractive(language == UiLanguage::Russian
        ? L"  0 - Отмена\n" : L"  0 - Cancel\n");

    std::size_t selected = 0;
    for (;;) {
        writeInteractive(language == UiLanguage::Russian
            ? L"Ваш выбор: " : L"Select: ");
        const std::wstring answer = trim(readInteractiveLine());
        try {
            const int parsed = parseInt(answer, "service selection");
            if (parsed == 0) {
                return 2;
            }
            if (parsed > 0 &&
                static_cast<std::size_t>(parsed) <= instances.size())
            {
                selected = static_cast<std::size_t>(parsed - 1);
                break;
            }
        } catch (...) {
        }
        writeInteractive(language == UiLanguage::Russian
            ? L"Введите номер из списка.\n" : L"Enter a number from the list.\n");
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write service selection");
    }
    output << "instance=" << utf8(instances[selected].instance) << "\r\n";
    output.close();
    if (!output) {
        throw std::runtime_error("cannot finish service selection");
    }
    return 0;
}

int chooseRecommendedOrManual(
    UiLanguage language,
    const std::wstring& russianTitle,
    const std::wstring& englishTitle,
    int recommended,
    int minimum,
    int maximum)
{
    for (;;) {
        const std::wstring title = language == UiLanguage::Russian
            ? russianTitle : englishTitle;
        const std::wstring recommendedLabel = language == UiLanguage::Russian
            ? L"Использовать рекомендуемое значение"
            : L"Use the recommended value";
        const std::wstring manualLabel = language == UiLanguage::Russian
            ? L"Ввести вручную" : L"Enter manually";
        const std::wstring selectLabel = language == UiLanguage::Russian
            ? L"Ваш выбор [1]: " : L"Select [1]: ";

        writeInteractive(
            L"\n" + title + L":\n  1 - " + recommendedLabel + L" (" +
            std::to_wstring(recommended) + L")\n  2 - " + manualLabel + L" (" +
            std::to_wstring(minimum) + L".." + std::to_wstring(maximum) +
            L")\n" + selectLabel
        );
        const std::wstring answer = trim(readInteractiveLine());
        if (answer.empty() || answer == L"1") {
            return recommended;
        }
        if (answer != L"2") {
            writeInteractive(language == UiLanguage::Russian
                ? L"Введите 1 или 2.\n" : L"Enter 1 or 2.\n");
            continue;
        }

        for (;;) {
            writeInteractive(language == UiLanguage::Russian
                ? L"Введите значение: " : L"Enter value: ");
            const std::wstring value = trim(readInteractiveLine());
            try {
                const int parsed = parseInt(value, "interactive value");
                if (parsed >= minimum && parsed <= maximum) {
                    return parsed;
                }
            } catch (...) {
            }
            writeInteractive(
                (language == UiLanguage::Russian
                    ? L"Недопустимое значение. Разрешён диапазон: "
                    : L"Invalid value. Allowed range: ") +
                std::to_wstring(minimum) + L".." +
                std::to_wstring(maximum) + L".\n"
            );
        }
    }
}

class Winsock final {
public:
    Winsock()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~Winsock() { WSACleanup(); }

    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
};

class Socket final {
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) : value_(value) {}
    ~Socket()
    {
        if (value_ != INVALID_SOCKET) {
            closesocket(value_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET get() const noexcept { return value_; }

private:
    SOCKET value_;
};

SystemInfo getSystemInfo()
{
    SYSTEM_INFO native{};
    GetSystemInfo(&native);
    const int logical = std::max(1, static_cast<int>(native.dwNumberOfProcessors));
    const int architectureCap = sizeof(void*) == 4 ? 32 : 64;
    const int recommended = std::max(2, std::min(logical, 8));

    SYSTEMTIME now{};
    GetLocalTime(&now);

    return {
        logical,
        recommended,
        std::max(2, std::min(logical * 2, architectureCap)),
        std::max(1, std::min(recommended - 1, 4)),
        std::max(1, std::min(logical, 4)),
        static_cast<int>(now.wYear)
    };
}

std::optional<std::wstring> option(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for command option");
            }
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::wstring requiredOption(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    const auto value = option(args, name);
    if (!value || value->empty()) {
        throw std::runtime_error("required command option is missing");
    }
    return *value;
}

int parseInt(const std::wstring& text, const char* field)
{
    std::size_t used = 0;
    long long value = 0;
    try {
        value = std::stoll(text, &used, 10);
    } catch (...) {
        throw std::runtime_error(std::string(field) + " is not an integer");
    }
    if (used != text.size() || value < INT_MIN || value > INT_MAX) {
        throw std::runtime_error(std::string(field) + " is not a valid integer");
    }
    return static_cast<int>(value);
}

bool parseBool(std::wstring text, const char* field)
{
    std::transform(text.begin(), text.end(), text.begin(), towlower);
    if (text == L"1" || text == L"true" || text == L"on" ||
        text == L"yes")
    {
        return true;
    }
    if (text == L"0" || text == L"false" || text == L"off" ||
        text == L"no")
    {
        return false;
    }
    throw std::runtime_error(std::string(field) + " must be 0 or 1");
}

json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open JSON file");
    }
    json value;
    input >> value;
    return value;
}

void mergeJson(json& destination, const json& source)
{
    if (!destination.is_object() || !source.is_object()) {
        destination = source;
        return;
    }
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (destination.contains(it.key())) {
            mergeJson(destination[it.key()], it.value());
        } else {
            destination[it.key()] = it.value();
        }
    }
}

void stripRetiredBlock1Fields(json& root)
{
    if (!root.is_object()) {
        return;
    }
    root.erase("Files");
    if (!root.contains("config") || !root["config"].is_object()) {
        return;
    }
    json& config = root["config"];
    if (!config.contains("hide_console_window") && config.contains("hide_mode")) {
        config["hide_console_window"] = config["hide_mode"];
    }
    for (const char* key : {
        "hide_mode",
        "dir",
        "Version",
        "text_request",
        "save_dictionary_to_file",
        "Name",
    }) {
        config.erase(key);
    }
}

void canonicalizeBlock1LegacySettings(json& root)
{
    stripRetiredBlock1Fields(root);
}

void migrateBlock2IndexRootsAliases(json& root)
{
    if (!root.is_object()) {
        return;
    }
    if (!root.contains("config") || !root["config"].is_object()) {
        return;
    }
    json& config = root["config"];
    if (!config.contains("index_roots") && config.contains("dirs")) {
        config["index_roots"] = config["dirs"];
    }
    config.erase("dirs");
    if (!config.contains("excluded_subtrees") && config.contains("exclude_dirs")) {
        config["excluded_subtrees"] = config["exclude_dirs"];
    }
    config.erase("exclude_dirs");
}

void canonicalizeLegacySettings(json& root)
{
    stripRetiredBlock1Fields(root);
    migrateBlock2IndexRootsAliases(root);
}

std::string withTrailingLf(std::string text)
{
    if (text.empty() || text.back() != '\n') {
        text.push_back('\n');
    }
    return text;
}

std::string makeCrLf(std::string_view text)
{
    std::string result;
    result.reserve(text.size() + 16);
    for (const char ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            result += "\r\n";
        } else {
            result.push_back(ch);
        }
    }
    if (result.size() < 2 ||
        result[result.size() - 2] != '\r' ||
        result.back() != '\n')
    {
        result += "\r\n";
    }
    return result;
}

std::string serializeJsonText(
    const json& value,
    int indent,
    JsonLineEnding lineEnding)
{
    const std::string dumped = value.dump(indent);
    if (lineEnding == JsonLineEnding::CrLf) {
        return makeCrLf(dumped);
    }
    return withTrailingLf(dumped);
}

JsonLineEnding parseJsonLineEnding(const std::wstring& text)
{
    if (text == L"crlf") {
        return JsonLineEnding::CrLf;
    }
    if (text == L"lf") {
        return JsonLineEnding::Lf;
    }
    throw std::runtime_error("line ending must be lf or crlf");
}

void writeJsonAtomically(
    const fs::path& output,
    const json& value,
    int indent = 2,
    JsonLineEnding lineEnding = JsonLineEnding::Lf)
{
    const std::string text = serializeJsonText(value, indent, lineEnding);
    fs::path temporary = output;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("cannot create temporary settings file");
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            throw std::runtime_error("cannot write temporary settings file");
        }
    }

    if (!MoveFileExW(
            temporary.c_str(),
            output.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::runtime_error(
            "cannot replace settings file; Win32 error " +
            std::to_string(error)
        );
    }
}

std::optional<long long> jsonInteger(const json& value)
{
    if (value.is_number_integer()) {
        return value.get<long long>();
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<unsigned long long>();
        if (number <= static_cast<unsigned long long>(LLONG_MAX)) {
            return static_cast<long long>(number);
        }
    }
    return std::nullopt;
}

std::vector<std::string> validateJson(const json& root, bool checkDirectories)
{
    std::vector<std::string> errors;
    if (!root.is_object() || !root.contains("config") ||
        !root["config"].is_object())
    {
        errors.emplace_back("config object is missing");
        return errors;
    }

    const json& config = root["config"];
    const auto requireString = [&](const char* name) {
        if (!config.contains(name) || !config[name].is_string() ||
            config[name].get_ref<const std::string&>().empty())
        {
            errors.emplace_back(std::string("config.") + name +
                                " must be a non-empty string");
        }
    };
    requireString("year");
    const auto requireBaseDirString = [&](const char* name) {
        if (!config.contains(name) || !config[name].is_string())
        {
            errors.emplace_back(std::string("config.") + name +
                                " must be a string (empty disables the source)");
        }
    };
    requireBaseDirString("prm_base_dir");
    requireBaseDirString("prd_base_dir");

    const auto requireAbsoluteLocalRoot = [&](const char* name) {
        if (!config.contains(name) || !config[name].is_string() ||
            config[name].get_ref<const std::string&>().empty())
        {
            errors.emplace_back(
                std::string("config.") + name + " must be a non-empty string");
            return;
        }
        const auto& value = config[name].get_ref<const std::string&>();
        if (!isAbsoluteWindowsLocalPath(value)) {
            errors.emplace_back(
                std::string("config.") + name +
                " must be an absolute local Windows path "
                "(UNC and relative paths are not supported)");
        }
    };
    requireAbsoluteLocalRoot("tlg_send_root");
    requireAbsoluteLocalRoot("razn_output_dir");
    requireAbsoluteLocalRoot("opis_base_dir");
    requireAbsoluteLocalRoot("f12_base_dir");

    if (!config.contains("index_roots") || !config["index_roots"].is_array() ||
        config["index_roots"].empty())
    {
        errors.emplace_back("config.index_roots must be a non-empty array");
    }
    if (config.contains("dirs")) {
        errors.emplace_back(
            "config.dirs is retired; use config.index_roots");
    }
    if (config.contains("exclude_dirs")) {
        errors.emplace_back(
            "config.exclude_dirs is retired; use config.excluded_subtrees");
    }
    if (!config.contains("extensions") ||
        !config["extensions"].is_array() || config["extensions"].empty())
    {
        errors.emplace_back("config.extensions must be a non-empty array");
    }

    const char* portName = config.contains("port") ? "port" : "asio_port";
    const auto port = config.contains(portName)
        ? jsonInteger(config[portName]) : std::nullopt;
    if (!port || *port < 1 || *port > 65535) {
        errors.emplace_back("config port (or asio_port) must be inside 1..65535");
    }

    if (config.contains("year") && config["year"].is_string()) {
        const std::string yearText = config["year"].get<std::string>();
        try {
            const int year = std::stoi(yearText);
            if (yearText.size() != 4 || year < kMinYear || year > kMaxYear) {
                errors.emplace_back("config.year must be inside 2000..2099");
            }
        } catch (...) {
            errors.emplace_back("config.year must contain four digits");
        }
    }

    const SystemInfo system = getSystemInfo();
    const auto threads = config.contains("thread_count")
        ? jsonInteger(config["thread_count"]) : std::nullopt;
    if (!threads || (*threads != 0 &&
        (*threads < 2 || *threads > system.maximumThreads)))
    {
        errors.emplace_back(
            "config.thread_count must be 0 or inside 2.." +
            std::to_string(system.maximumThreads)
        );
    }

    const auto timeout = config.contains("file_indexing_timeout_sec")
        ? jsonInteger(config["file_indexing_timeout_sec"]) : std::nullopt;
    if (!timeout || *timeout < kMinFileTimeout ||
        *timeout > kMaxFileTimeout)
    {
        errors.emplace_back(
            "config.file_indexing_timeout_sec must be inside 10..600"
        );
    }

    if (config.contains("full_index_strategy")) {
        if (!config["full_index_strategy"].is_string() ||
            !inverted_index::parseFullIndexStrategy(
                config["full_index_strategy"].get<std::string>()))
        {
            errors.emplace_back(
                "config.full_index_strategy must be legacy or batch");
        }
    }

    if (config.contains("document_catalog_storage")) {
        if (!config["document_catalog_storage"].is_string() ||
            !inverted_index::parseDocumentCatalogStorage(
                config["document_catalog_storage"].get<std::string>()))
        {
            errors.emplace_back(
                "config.document_catalog_storage must be memory or sqlite");
        }
    }

    if (config.contains("batch_reader_threads")) {
        const auto readers = jsonInteger(config["batch_reader_threads"]);
        if (!readers || *readers < 1 || *readers > 64) {
            errors.emplace_back(
                "config.batch_reader_threads must be inside 1..64");
        }
    }

    if (config.contains("batch_indexer_threads")) {
        const auto indexers = jsonInteger(config["batch_indexer_threads"]);
        if (!indexers || *indexers < 0 || *indexers > 256) {
            errors.emplace_back(
                "config.batch_indexer_threads must be 0 or inside 1..256");
        }
    }

    if (config.contains("batch_queue_memory_mb")) {
        const auto memory = jsonInteger(config["batch_queue_memory_mb"]);
        if (!memory || *memory < 16 || *memory > 2048) {
            errors.emplace_back(
                "config.batch_queue_memory_mb must be inside 16..2048");
        }
    }

    if (!config.contains("enable_prm_short_content_autodetect") ||
        !config["enable_prm_short_content_autodetect"].is_boolean())
    {
        errors.emplace_back(
            "config.enable_prm_short_content_autodetect must be boolean"
        );
    }

    if (config.contains("scan_on_startup") &&
        !config["scan_on_startup"].is_boolean())
    {
        errors.emplace_back("config.scan_on_startup must be boolean");
    }

    // SVC-001: validate additional fields deserialized by ConverterJSON

    // Boolean fields: if present must be booleans (get_to<bool> would throw otherwise).
    for (const char* boolField : {
        "exact_search", "hide_console_window", "scan_on_startup"
    }) {
        if (config.contains(boolField) && !config[boolField].is_boolean()) {
            errors.emplace_back(std::string("config.") + boolField + " must be boolean");
        }
    }

    // Element type checks for index_roots and extensions.
    for (const char* arrField : {"index_roots", "extensions"}) {
        if (config.contains(arrField) && config[arrField].is_array()) {
            for (const auto& el : config[arrField]) {
                if (!el.is_string()) {
                    errors.emplace_back(
                        std::string("config.") + arrField + " must contain only strings");
                    break;
                }
            }
        }
    }

    if (config.contains("index_roots") && config["index_roots"].is_array()) {
        for (const auto& el : config["index_roots"]) {
            if (!el.is_string()) {
                continue;
            }
            const auto& value = el.get_ref<const std::string&>();
            if (value.empty()) {
                errors.emplace_back(
                    "config.index_roots must contain non-empty strings");
                break;
            }
            if (!isAbsoluteWindowsFilesystemPath(value)) {
                errors.emplace_back(
                    "config.index_roots must contain absolute Windows paths "
                    "(local drive or UNC; relative paths are not supported)");
                break;
            }
        }
    }

    // excluded_subtrees: if present must be array of strings.
    if (config.contains("excluded_subtrees")) {
        if (!config["excluded_subtrees"].is_array()) {
            errors.emplace_back("config.excluded_subtrees must be an array");
        } else {
            for (const auto& el : config["excluded_subtrees"]) {
                if (!el.is_string()) {
                    errors.emplace_back(
                        "config.excluded_subtrees must contain only strings");
                    break;
                }
                const auto& value = el.get_ref<const std::string&>();
                if (value.empty() || !isAbsoluteWindowsFilesystemPath(value)) {
                    errors.emplace_back(
                        "config.excluded_subtrees must contain non-empty "
                        "absolute Windows paths "
                        "(local drive or UNC; relative paths are not supported)");
                    break;
                }
            }
        }
    }

    // max_response: runtime uses it as-is; 0 returns 0 results (valid operational value).
    if (config.contains("max_response")) {
        const auto v = jsonInteger(config["max_response"]);
        if (!v || *v < 0 || *v > std::numeric_limits<int>::max()) {
            errors.emplace_back("config.max_response must be a non-negative integer");
        }
    }

    if (config.contains("ind_time")) {
        // Re-index period in seconds; 0 is not valid at runtime.
        const auto v = jsonInteger(config["ind_time"]);
        if (!v || *v < 1) {
            errors.emplace_back("config.ind_time must be integer >= 1");
        } else {
            // ind_time is parsed into size_t at runtime. Validate representable range.
            const unsigned long long uv =
                static_cast<unsigned long long>(*v);
            const unsigned long long maxSizeT =
                static_cast<unsigned long long>(
                    std::numeric_limits<size_t>::max());
            if (uv > maxSizeT) {
                errors.emplace_back("config.ind_time is out of representable size_t range");
            }
        }
    }

    if (config.contains("max_parallel_readers")) {
        // 0 = no limit (runtime default); positive values cap concurrency.
        const auto v = jsonInteger(config["max_parallel_readers"]);
        if (!v || *v < 0 || *v > std::numeric_limits<int>::max()) {
            errors.emplace_back(
                "config.max_parallel_readers must be a non-negative integer");
        }
    }

    if (config.contains("compact_threshold_percent")) {
        const auto& fld = config["compact_threshold_percent"];
        if (!fld.is_number()) {
            errors.emplace_back("config.compact_threshold_percent must be a number");
        } else {
            const double pct = fld.get<double>();
            if (pct < 0.0 || pct > 100.0) {
                errors.emplace_back("config.compact_threshold_percent must be inside 0..100");
            }
        }
    }

    if (config.contains("sqlite_mirror_flush_interval_sec")) {
        // Accepts float (e.g. 2.0); must be a positive number.
        const auto& fld = config["sqlite_mirror_flush_interval_sec"];
        if (!fld.is_number()) {
            errors.emplace_back(
                "config.sqlite_mirror_flush_interval_sec must be a number");
        } else if (fld.get<double>() <= 0.0) {
            errors.emplace_back(
                "config.sqlite_mirror_flush_interval_sec must be > 0");
        }
    }

    if (config.contains("sqlite_mirror_max_pending_ops")) {
        // 0 = flush by timer only (documented operational value).
        const auto v = jsonInteger(config["sqlite_mirror_max_pending_ops"]);
        if (!v || *v < 0 || *v > std::numeric_limits<int>::max()) {
            errors.emplace_back(
                "config.sqlite_mirror_max_pending_ops must be a non-negative integer");
        }
    }

    if (config.contains("sqlite_load_threads")) {
        // Must be >= 1 (0 has no defined runtime semantics for parallelism degree).
        const auto v = jsonInteger(config["sqlite_load_threads"]);
        if (!v || *v < 1 || *v > std::numeric_limits<int>::max()) {
            errors.emplace_back("config.sqlite_load_threads must be integer >= 1");
        }
    }

    if (config.contains("sqlite_precount_postings") &&
        !config["sqlite_precount_postings"].is_boolean())
    {
        errors.emplace_back("config.sqlite_precount_postings must be boolean");
    }

    if (checkDirectories && config.contains("index_roots") &&
        config["index_roots"].is_array())
    {
        for (const auto& directory : config["index_roots"]) {
            if (!directory.is_string() ||
                !fs::is_directory(fs::u8path(directory.get<std::string>())))
            {
                errors.emplace_back("configured indexing directory is missing");
            }
        }
    }
    if (checkDirectories) {
        for (const char* name : {"prm_base_dir", "prd_base_dir"}) {
            if (!config.contains(name) || !config[name].is_string())
                continue;
            const auto& value = config[name].get_ref<const std::string&>();
            if (value.empty())
                continue;
            if (!fs::is_directory(fs::u8path(value)))
            {
                errors.emplace_back(
                    std::string("configured ") + name + " directory is missing"
                );
            }
        }
    }
    return errors;
}

void printSystemInfo()
{
    const SystemInfo info = getSystemInfo();
    std::cout << "architecture=" << (sizeof(void*) == 4 ? "x86" : "x64") << '\n'
              << "logical_processors=" << info.logicalProcessors << '\n'
              << "recommended_threads=" << info.recommendedThreads << '\n'
              << "maximum_threads=" << info.maximumThreads << '\n'
              << "recommended_parallel_readers=" << info.recommendedReaders << '\n'
              << "recommended_sqlite_load_threads="
              << info.recommendedSqliteThreads << '\n'
              << "recommended_file_timeout_sec="
              << kRecommendedFileTimeout << '\n'
              << "file_timeout_min_sec=" << kMinFileTimeout << '\n'
              << "file_timeout_max_sec=" << kMaxFileTimeout << '\n'
              << "current_year=" << info.currentYear << '\n'
              << "year_min=" << kMinYear << '\n'
              << "year_max=" << kMaxYear << '\n';
}

int formatJsonCommand(const std::vector<std::wstring>& args)
{
    const fs::path settings = requiredOption(args, L"--settings");
    JsonLineEnding lineEnding = JsonLineEnding::CrLf;
    if (const auto value = option(args, L"--line-ending")) {
        lineEnding = parseJsonLineEnding(*value);
    }
    const json root = readJson(settings);
    writeJsonAtomically(settings, root, kFormatJsonIndent, lineEnding);
    return 0;
}

int compareJsonCommand(const std::vector<std::wstring>& args)
{
    const fs::path left = requiredOption(args, L"--left");
    const fs::path right = requiredOption(args, L"--right");
    return readJson(left) == readJson(right) ? 0 : 1;
}

int validateCommand(const std::vector<std::wstring>& args)
{
    const fs::path settings = requiredOption(args, L"--settings");
    const bool checkDirectories =
        std::find(args.begin(), args.end(), L"--check-dirs") != args.end();
    json root = readJson(settings);
    canonicalizeLegacySettings(root);
    const auto errors = validateJson(root, checkDirectories);
    for (const auto& error : errors) {
        std::cout << "error=" << error << '\n';
    }
    std::cout << "settings_valid=" << (errors.empty() ? 1 : 0) << '\n';
    return errors.empty() ? 0 : 2;
}

int validatePrefixMapCommand(const std::vector<std::wstring>& args)
{
    const fs::path path = requiredOption(args, L"--path");
    std::vector<std::string> errors;

    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) {
        errors.emplace_back("prefix map file does not exist");
    } else if (!fs::is_regular_file(status)) {
        errors.emplace_back("prefix map path is not a regular file");
    } else {
        json root;
        bool parsed = false;
        try {
            root = readJson(path);
            parsed = true;
        } catch (const json::parse_error&) {
            errors.emplace_back("prefix map JSON is invalid");
        } catch (const std::exception& exception) {
            errors.emplace_back(exception.what());
        }

        if (parsed) {
            if (!root.is_object()) {
                errors.emplace_back("prefix map root must be an object");
            } else {
                if (!root.contains("prefix")) {
                    errors.emplace_back("prefix map is missing prefix");
                } else if (!root["prefix"].is_string()) {
                    errors.emplace_back("prefix map prefix must be a string");
                }

                if (!root.contains("map")) {
                    errors.emplace_back("prefix map is missing map");
                } else if (!root["map"].is_object()) {
                    errors.emplace_back("prefix map map must be an object");
                } else {
                    for (const auto& item : root["map"].items()) {
                        if (!item.value().is_string()) {
                            errors.emplace_back(
                                "prefix map map values must be strings");
                            break;
                        }
                    }
                }
            }
        }
    }

    for (const auto& error : errors) {
        std::cout << "error=" << error << '\n';
    }
    std::cout << "prefix_map_valid=" << (errors.empty() ? 1 : 0) << '\n';
    return errors.empty() ? 0 : 2;
}

int inspectCommand(const std::vector<std::wstring>& args)
{
    json root = readJson(requiredOption(args, L"--settings"));
    canonicalizeLegacySettings(root);
    const auto errors = validateJson(root, false);
    if (!root.contains("config") || !root["config"].is_object()) {
        return validateCommand(args);
    }
    const json& config = root["config"];
    const char* portName = config.contains("port") ? "port" : "asio_port";
    std::cout << "port=" << config.value(portName, 0) << '\n'
              << "year=" << config.value("year", std::string()) << '\n'
              << "threads=" << config.value("thread_count", 0) << '\n'
              << "file_timeout_sec="
              << config.value("file_indexing_timeout_sec", 0) << '\n'
              << "parallel_readers="
              << config.value("max_parallel_readers", 0) << '\n'
              << "full_index_strategy="
              << config.value(
                    "full_index_strategy", std::string("batch")) << '\n'
              << "document_catalog_storage="
              << config.value(
                    "document_catalog_storage", std::string("memory")) << '\n'
              << "batch_reader_threads="
              << config.value("batch_reader_threads", 1) << '\n'
              << "batch_indexer_threads="
              << config.value("batch_indexer_threads", 0) << '\n'
              << "batch_queue_memory_mb="
              << config.value("batch_queue_memory_mb", 256) << '\n'
              << "sqlite_load_threads="
              << config.value("sqlite_load_threads", 0) << '\n'
              << "prm_short_content_autodetect="
              << (config.value(
                    "enable_prm_short_content_autodetect", true) ? 1 : 0)
              << '\n'
              << "scan_on_startup="
              << (config.value("scan_on_startup", true) ? 1 : 0)
              << '\n'
              << "tlg_send_root="
              << config.value("tlg_send_root", std::string()) << '\n'
              << "razn_output_dir="
              << config.value("razn_output_dir", std::string()) << '\n'
              << "opis_base_dir="
              << config.value("opis_base_dir", std::string()) << '\n'
              << "f12_base_dir="
              << config.value("f12_base_dir", std::string()) << '\n'
              << "settings_valid=" << (errors.empty() ? 1 : 0) << '\n';
    return errors.empty() ? 0 : 2;
}

int configureCommand(const std::vector<std::wstring>& args)
{
    const fs::path templatePath = requiredOption(args, L"--template");
    const fs::path outputPath = requiredOption(args, L"--output");
    const int port = parseInt(requiredOption(args, L"--port"), "port");
    const int year = parseInt(requiredOption(args, L"--year"), "year");
    const int threads = parseInt(requiredOption(args, L"--threads"), "threads");
    const int timeout = parseInt(
        requiredOption(args, L"--file-timeout"), "file timeout");
    const bool prmAutodetect = parseBool(
        requiredOption(args, L"--prm-autodetect"), "PRM autodetect");

    const SystemInfo system = getSystemInfo();
    if (port < 1 || port > 65535) {
        throw std::runtime_error("port must be inside 1..65535");
    }
    if (year < kMinYear || year > kMaxYear) {
        throw std::runtime_error("year must be inside 2000..2099");
    }
    if (threads < 2 || threads > system.maximumThreads) {
        throw std::runtime_error(
            "threads must be inside 2.." +
            std::to_string(system.maximumThreads)
        );
    }
    if (timeout < kMinFileTimeout || timeout > kMaxFileTimeout) {
        throw std::runtime_error("file timeout must be inside 10..600");
    }

    int readers = std::max(1, std::min(threads - 1, 4));
    if (const auto value = option(args, L"--parallel-readers")) {
        readers = parseInt(*value, "parallel readers");
    }
    if (readers < 1 || readers > std::max(1, threads - 1)) {
        throw std::runtime_error(
            "parallel readers must be inside 1..threads-1"
        );
    }

    int sqliteThreads = system.recommendedSqliteThreads;
    if (const auto value = option(args, L"--sqlite-load-threads")) {
        sqliteThreads = parseInt(*value, "SQLite load threads");
    }
    if (sqliteThreads < 1 || sqliteThreads > std::min(system.logicalProcessors, 8)) {
        throw std::runtime_error(
            "SQLite load threads must be inside 1..min(logical processors, 8)"
        );
    }

    json result = readJson(templatePath);
    if (const auto importPath = option(args, L"--import-settings")) {
        json imported = readJson(fs::path(*importPath));
        canonicalizeLegacySettings(imported);
        mergeJson(result, imported);
    }
    if (!result.contains("config") || !result["config"].is_object()) {
        result["config"] = json::object();
    }
    json& config = result["config"];
    std::string documentCatalogStorage = config.value(
        "document_catalog_storage", std::string("memory"));
    if (const auto value = option(args, L"--document-catalog-storage")) {
        documentCatalogStorage = utf8(*value);
    }
    if (!inverted_index::parseDocumentCatalogStorage(documentCatalogStorage)) {
        throw std::runtime_error(
            "document catalog storage must be memory or sqlite");
    }
    config.erase("port");
    config["asio_port"] = port;
    config["year"] = std::to_string(year);
    config["thread_count"] = threads;
    config["max_parallel_readers"] = readers;
    config["sqlite_load_threads"] = sqliteThreads;
    config["file_indexing_timeout_sec"] = timeout;
    config["enable_prm_short_content_autodetect"] = prmAutodetect;
    config["document_catalog_storage"] = documentCatalogStorage;
    if (const auto value = option(args, L"--tlg-send-root")) {
        config["tlg_send_root"] = utf8(*value);
    }
    if (const auto value = option(args, L"--razn-output-dir")) {
        config["razn_output_dir"] = utf8(*value);
    }
    if (const auto value = option(args, L"--opis-base-dir")) {
        config["opis_base_dir"] = utf8(*value);
    }
    if (const auto value = option(args, L"--f12-base-dir")) {
        config["f12_base_dir"] = utf8(*value);
    }

    canonicalizeLegacySettings(result);

    const auto errors = validateJson(result, false);
    if (!errors.empty()) {
        for (const auto& error : errors) {
            std::cerr << "ERROR: " << error << '\n';
        }
        return 2;
    }
    writeJsonAtomically(outputPath, result);
    if (std::find(args.begin(), args.end(), L"--quiet") == args.end()) {
        std::cout << "settings_written=1\n"
                  << "port=" << port << '\n'
                  << "year=" << year << '\n'
                  << "threads=" << threads << '\n'
                  << "file_timeout_sec=" << timeout << '\n'
                  << "parallel_readers=" << readers << '\n'
                  << "sqlite_load_threads=" << sqliteThreads << '\n'
                  << "document_catalog_storage="
                  << documentCatalogStorage << '\n'
                  << "tlg_send_root="
                  << config.value("tlg_send_root", std::string()) << '\n'
                  << "razn_output_dir="
                  << config.value("razn_output_dir", std::string()) << '\n'
                  << "opis_base_dir="
                  << config.value("opis_base_dir", std::string()) << '\n'
                  << "f12_base_dir="
                  << config.value("f12_base_dir", std::string()) << '\n'
                  << "prm_short_content_autodetect="
                  << (prmAutodetect ? 1 : 0) << '\n';
    }
    return 0;
}

bool choosePrmAutodetect(UiLanguage language)
{
    for (;;) {
        writeInteractive(language == UiLanguage::Russian
            ? L"\nАвтоматически заполнять краткое содержание AutoPad PRM:\n"
              L"  1 - Включить (по умолчанию)\n"
              L"  2 - Отключить\n"
              L"Ваш выбор [1]: "
            : L"\nAutomatically fill AutoPad PRM short content:\n"
              L"  1 - Enable (default)\n"
              L"  2 - Disable\n"
              L"Select [1]: ");
        const std::wstring answer = trim(readInteractiveLine());
        if (answer.empty() || answer == L"1") {
            return true;
        }
        if (answer == L"2") {
            return false;
        }
        writeInteractive(language == UiLanguage::Russian
            ? L"Введите 1 или 2.\n" : L"Enter 1 or 2.\n");
    }
}

std::string chooseDocumentCatalogStorage(
    UiLanguage language,
    std::string current)
{
    if (!inverted_index::parseDocumentCatalogStorage(current))
        current = "memory";
    const std::wstring defaultChoice = current == "sqlite" ? L"2" : L"1";
    for (;;) {
        writeInteractive(language == UiLanguage::Russian
            ? L"\nГде хранить каталог документов (пути и метаданные)?\n"
              L"  1 - В оперативной памяти — быстрее (по умолчанию)\n"
              L"  2 - В SQLite — меньше расход RAM, возможна небольшая задержка\n"
              L"Ваш выбор [" + defaultChoice + L"]: "
            : L"\nWhere should the document catalog (paths and metadata) be stored?\n"
              L"  1 - In memory — faster (default)\n"
              L"  2 - In SQLite — lower RAM use, with possible small latency\n"
              L"Select [" + defaultChoice + L"]: ");
        const std::wstring answer = trim(readInteractiveLine());
        if (answer.empty())
            return current;
        if (answer == L"1")
            return "memory";
        if (answer == L"2")
            return "sqlite";
        writeInteractive(language == UiLanguage::Russian
            ? L"Введите 1 или 2.\n" : L"Enter 1 or 2.\n");
    }
}

std::string configStringOr(
    const json& config,
    const char* name,
    const std::string& fallback)
{
    if (config.contains(name) && config[name].is_string()) {
        const auto& value = config[name].get_ref<const std::string&>();
        if (!value.empty()) {
            return value;
        }
    }
    return fallback;
}

std::string chooseAbsoluteLocalPath(
    UiLanguage language,
    const wchar_t* ruTitle,
    const wchar_t* enTitle,
    const wchar_t* ruPurpose,
    const wchar_t* enPurpose,
    const std::string& current)
{
    for (;;) {
        writeInteractive(
            std::wstring(L"\n") +
            (language == UiLanguage::Russian ? ruTitle : enTitle) + L"\n" +
            (language == UiLanguage::Russian ? ruPurpose : enPurpose) + L"\n" +
            (language == UiLanguage::Russian
                ? L"Текущее значение: "
                : L"Current value: ") +
            utf16(current) + L"\n" +
            (language == UiLanguage::Russian
                ? L"Абсолютный локальный путь Windows [Enter = оставить]: "
                : L"Absolute local Windows path [Enter = keep]: "));
        const std::wstring answer = trim(readInteractiveLine());
        const std::string candidate = answer.empty() ? current : utf8(answer);
        if (candidate.empty()) {
            writeInteractive(language == UiLanguage::Russian
                ? L"Путь не должен быть пустым.\n"
                : L"The path must not be empty.\n");
            continue;
        }
        if (!isAbsoluteWindowsLocalPath(candidate)) {
            writeInteractive(language == UiLanguage::Russian
                ? L"Нужен абсолютный локальный путь (например E:\\OPIS_ADMIN). "
                  L"UNC и относительные пути не принимаются.\n"
                : L"Enter an absolute local path (for example E:\\OPIS_ADMIN). "
                  L"UNC and relative paths are not accepted.\n");
            continue;
        }
        return candidate;
    }
}

int configureInteractiveCommand(const std::vector<std::wstring>& args)
{
    const fs::path templatePath = requiredOption(args, L"--template");
    const fs::path outputPath = requiredOption(args, L"--output");
    json defaults = readJson(templatePath);
    if (const auto importPath = option(args, L"--import-settings")) {
        json imported = readJson(fs::path(*importPath));
        canonicalizeLegacySettings(imported);
        mergeJson(defaults, imported);
    }

    int defaultPort = 15001;
    std::string defaultDocumentCatalogStorage = "memory";
    std::string defaultTlgSendRoot = "D:\\";
    std::string defaultRaznOutputDir =
        "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ";
    std::string defaultOpisBaseDir = "D:\\OPIS_ADMIN";
    std::string defaultF12BaseDir = "D:\\F12";
    if (defaults.contains("config") && defaults["config"].is_object()) {
        const json& config = defaults["config"];
        const char* portName = config.contains("asio_port") ? "asio_port" : "port";
        if (config.contains(portName)) {
            if (const auto value = jsonInteger(config[portName]);
                value && *value >= 1 && *value <= 65535)
            {
                defaultPort = static_cast<int>(*value);
            }
        }
        if (config.contains("document_catalog_storage") &&
            config["document_catalog_storage"].is_string())
        {
            const std::string candidate =
                config["document_catalog_storage"].get<std::string>();
            if (inverted_index::parseDocumentCatalogStorage(candidate))
                defaultDocumentCatalogStorage = candidate;
        }
        defaultTlgSendRoot = configStringOr(
            config, "tlg_send_root", defaultTlgSendRoot);
        defaultRaznOutputDir = configStringOr(
            config, "razn_output_dir", defaultRaznOutputDir);
        defaultOpisBaseDir = configStringOr(
            config, "opis_base_dir", defaultOpisBaseDir);
        defaultF12BaseDir = configStringOr(
            config, "f12_base_dir", defaultF12BaseDir);
    }

    const UiLanguage language = interactiveLanguage(args);
    const SystemInfo system = getSystemInfo();
    const int port = chooseRecommendedOrManual(
        language, L"ASIO TCP-порт", L"ASIO TCP port",
        defaultPort, 1, 65535
    );
    const int year = chooseRecommendedOrManual(
        language, L"Рабочий год", L"Working year",
        system.currentYear, kMinYear, kMaxYear
    );
    const int threads = chooseRecommendedOrManual(
        language, L"Исполнительные потоки", L"Executor threads",
        system.recommendedThreads, 2, system.maximumThreads
    );
    const int timeout = chooseRecommendedOrManual(
        language, L"Тайм-аут индексации одного файла (секунды)",
        L"One-file indexing timeout (seconds)",
        kRecommendedFileTimeout, kMinFileTimeout, kMaxFileTimeout
    );
    const bool prmAutodetect = choosePrmAutodetect(language);
    const std::string documentCatalogStorage =
        chooseDocumentCatalogStorage(
            language, defaultDocumentCatalogStorage);
    const std::string tlgSendRoot = chooseAbsoluteLocalPath(
        language,
        L"Корень LOAD_TLG_TO_SEND (tlg_send_root)",
        L"LOAD_TLG_TO_SEND root (tlg_send_root)",
        L"Файлы сохраняются в <корень>\\<МЕСЯЦ>\\<ДАТА>\\. "
        L"Каталог сейчас может отсутствовать.",
        L"Files are stored under <root>\\<MONTH>\\<DATE>\\. "
        L"The directory does not have to exist yet.",
        defaultTlgSendRoot);
    const std::string raznOutputDir = chooseAbsoluteLocalPath(
        language,
        L"Каталог LOAD_RAZN (razn_output_dir)",
        L"LOAD_RAZN directory (razn_output_dir)",
        L"Куда писать разноску. Каталог сейчас может отсутствовать.",
        L"Destination for raznoska files. The directory does not have to exist yet.",
        defaultRaznOutputDir);
    const std::string opisBaseDir = chooseAbsoluteLocalPath(
        language,
        L"Каталог OPIS (opis_base_dir)",
        L"OPIS directory (opis_base_dir)",
        L"GET_OPIS_BASE читает <каталог>\\<год>.db. "
        L"RecordProcessor использует <каталог>\\<год>.DB.",
        L"GET_OPIS_BASE reads <directory>\\<year>.db. "
        L"RecordProcessor uses <directory>\\<year>.DB.",
        defaultOpisBaseDir);
    const std::string f12BaseDir = chooseAbsoluteLocalPath(
        language,
        L"Каталог F12 (f12_base_dir)",
        L"F12 directory (f12_base_dir)",
        L"GET_VH_TELEGA_WAY / GET_ISH_TELEGA_WAY читают "
        L"<каталог>\\<год>.db и <каталог>\\base.db.",
        L"GET_VH_TELEGA_WAY / GET_ISH_TELEGA_WAY read "
        L"<directory>\\<year>.db and <directory>\\base.db.",
        defaultF12BaseDir);

    std::vector<std::wstring> configureArgs{
        L"--template", templatePath.wstring(),
        L"--output", outputPath.wstring(),
        L"--port", std::to_wstring(port),
        L"--year", std::to_wstring(year),
        L"--threads", std::to_wstring(threads),
        L"--file-timeout", std::to_wstring(timeout),
        L"--prm-autodetect", prmAutodetect ? L"1" : L"0",
        L"--document-catalog-storage",
        documentCatalogStorage == "sqlite" ? L"sqlite" : L"memory",
        L"--tlg-send-root", utf16(tlgSendRoot),
        L"--razn-output-dir", utf16(raznOutputDir),
        L"--opis-base-dir", utf16(opisBaseDir),
        L"--f12-base-dir", utf16(f12BaseDir),
        L"--quiet"
    };
    if (const auto importPath = option(args, L"--import-settings")) {
        configureArgs.emplace_back(L"--import-settings");
        configureArgs.emplace_back(*importPath);
    }
    const int result = configureCommand(configureArgs);
    if (result != 0) {
        return result;
    }

    writeInteractive(language == UiLanguage::Russian
        ? L"\nНастройки сохранены.\n"
        : L"\nSettings have been saved.\n");
    writeInteractive(
        (language == UiLanguage::Russian ? L"  Порт: " : L"  Port: ") +
        std::to_wstring(port) + L"\n" +
        (language == UiLanguage::Russian ? L"  Год: " : L"  Year: ") +
        std::to_wstring(year) + L"\n" +
        (language == UiLanguage::Russian ? L"  Потоки: " : L"  Threads: ") +
        std::to_wstring(threads) + L"\n" +
        (language == UiLanguage::Russian ? L"  Тайм-аут файла: " : L"  File timeout: ") +
        std::to_wstring(timeout) +
        (language == UiLanguage::Russian ? L" сек.\n" : L" sec.\n") +
        (language == UiLanguage::Russian ? L"  AutoPad PRM: " : L"  AutoPad PRM: ") +
         (prmAutodetect
            ? (language == UiLanguage::Russian ? L"включено\n" : L"enabled\n")
            : (language == UiLanguage::Russian ? L"отключено\n" : L"disabled\n")) +
        (language == UiLanguage::Russian
            ? L"  Каталог документов: "
            : L"  Document catalog: ") +
        (documentCatalogStorage == "sqlite" ? L"SQLite\n" :
            (language == UiLanguage::Russian
                ? L"оперативная память\n" : L"memory\n")) +
        L"  tlg_send_root=" + utf16(tlgSendRoot) + L"\n" +
        L"  razn_output_dir=" + utf16(raznOutputDir) + L"\n" +
        L"  opis_base_dir=" + utf16(opisBaseDir) + L"\n" +
        L"  f12_base_dir=" + utf16(f12BaseDir) + L"\n"
    );
    return 0;
}

std::wstring normalizedPathKey(const fs::path& path)
{
    std::wstring value = fs::absolute(path).lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    while (value.size() > 3 &&
        (value.back() == L'\\' || value.back() == L'/'))
    {
        value.pop_back();
    }
    return value;
}

bool isSameOrBelow(const fs::path& candidate, const fs::path& parent)
{
    const std::wstring childKey = normalizedPathKey(candidate);
    std::wstring parentKey = normalizedPathKey(parent);
    if (childKey == parentKey) {
        return true;
    }
    parentKey += L'\\';
    return childKey.size() > parentKey.size() &&
        childKey.compare(0, parentKey.size(), parentKey) == 0;
}

std::uintmax_t treeSize(const fs::path& root)
{
    if (!fs::exists(root)) {
        return 0;
    }
    std::uintmax_t total = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, error), end;
         it != end;
         it.increment(error))
    {
        if (error) {
            error.clear();
            continue;
        }
        if (it->is_regular_file(error)) {
            total += it->file_size(error);
            error.clear();
        }
    }
    return total;
}

void copyTree(const fs::path& source, const fs::path& destination)
{
    if (!fs::exists(source)) {
        return;
    }
    fs::create_directories(destination);
    for (const auto& entry : fs::recursive_directory_iterator(
            source, fs::directory_options::skip_permission_denied))
    {
        const fs::path target = destination / fs::relative(entry.path(), source);
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_symlink()) {
            fs::create_directories(target.parent_path());
            fs::copy(
                entry.path(), target,
                fs::copy_options::copy_symlinks |
                    fs::copy_options::overwrite_existing
            );
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(
                entry.path(), target, fs::copy_options::overwrite_existing
            );
        }
    }
}

void copyFileIfPresent(const fs::path& source, const fs::path& destination)
{
    if (!fs::is_regular_file(source)) {
        return;
    }
    fs::create_directories(destination.parent_path());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
}

int backupCommand(const std::vector<std::wstring>& args)
{
    const fs::path installRoot = requiredOption(args, L"--install-root");
    const fs::path dataDir = requiredOption(args, L"--data-dir");
    const fs::path destinationBase = requiredOption(args, L"--destination");
    const std::wstring mode = requiredOption(args, L"--mode");
    if (mode != L"full" && mode != L"settings-logs") {
        throw std::runtime_error("backup mode must be full or settings-logs");
    }
    if (!fs::exists(installRoot) && !fs::exists(dataDir)) {
        throw std::runtime_error("there are no installed files to back up");
    }
    if (isSameOrBelow(destinationBase, installRoot) ||
        isSameOrBelow(destinationBase, dataDir))
    {
        throw std::runtime_error(
            "backup destination must be outside application and data directories"
        );
    }

    fs::create_directories(destinationBase);
    std::uintmax_t requiredBytes = 0;
    if (mode == L"full") {
        requiredBytes = treeSize(installRoot) + treeSize(dataDir);
    } else {
        requiredBytes = treeSize(dataDir / L"logs");
        for (const wchar_t* name : {L"Settings.json", L"ignore.txt", L"OEM866.INI"}) {
            std::error_code error;
            const auto fileBytes = fs::file_size(dataDir / name, error);
            if (!error) {
                requiredBytes += fileBytes;
            }
        }
    }
    const auto space = fs::space(destinationBase);
    if (space.available < requiredBytes + 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("backup destination does not have enough free space");
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t folderName[128]{};
    swprintf_s(
        folderName,
        L"SearchEngineService-backup-%04u%02u%02u-%02u%02u%02u",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond
    );
    fs::path finalPath = destinationBase / folderName;
    if (fs::exists(finalPath)) {
        finalPath += L"-" + std::to_wstring(GetCurrentProcessId());
    }
    fs::path stagingPath = finalPath;
    stagingPath += L".partial";
    if (fs::exists(stagingPath)) {
        throw std::runtime_error("backup staging directory already exists");
    }

    try {
        fs::create_directories(stagingPath);
        if (mode == L"full") {
            copyTree(installRoot, stagingPath / L"application");
            copyTree(dataDir, stagingPath / L"data");
        } else {
            const fs::path backupData = stagingPath / L"data";
            copyFileIfPresent(
                dataDir / L"Settings.json", backupData / L"Settings.json");
            copyFileIfPresent(
                dataDir / L"ignore.txt", backupData / L"ignore.txt");
            copyFileIfPresent(
                dataDir / L"OEM866.INI", backupData / L"OEM866.INI");
            copyTree(dataDir / L"logs", backupData / L"logs");
        }
        std::ofstream marker(stagingPath / L"backup-info.txt", std::ios::binary);
        marker << "SearchEngineService backup\r\nmode="
               << (mode == L"full" ? "full" : "settings-logs")
               << "\r\n";
        marker.close();
        fs::rename(stagingPath, finalPath);
    } catch (...) {
        std::error_code cleanupError;
        fs::remove_all(stagingPath, cleanupError);
        throw;
    }

    std::wcout << L"backup_created=1\nbackup_path="
               << finalPath.wstring() << L'\n';
    return 0;
}

sockaddr_in loopbackEndpoint(int port)
{
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<unsigned short>(port));
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return endpoint;
}

int checkPortCommand(const std::vector<std::wstring>& args)
{
    const int port = parseInt(requiredOption(args, L"--port"), "port");
    if (port < 1 || port > 65535) {
        throw std::runtime_error("port must be inside 1..65535");
    }
    Winsock winsock;
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.get() == INVALID_SOCKET) {
        throw std::runtime_error("cannot create port-check socket");
    }
    BOOL exclusive = TRUE;
    setsockopt(
        socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in endpoint = loopbackEndpoint(port);
    const bool available = bind(
        socket.get(), reinterpret_cast<sockaddr*>(&endpoint),
        sizeof(endpoint)) == 0;
    std::cout << "port=" << port << '\n'
              << "port_available=" << (available ? 1 : 0) << '\n';
    return available ? 0 : 3;
}

void connectWithTimeout(SOCKET socket, const sockaddr_in& endpoint, int timeoutMs)
{
    u_long nonBlocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) != 0) {
        throw std::runtime_error("cannot enable non-blocking socket mode");
    }
    const int result = connect(
        socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            throw std::runtime_error("cannot connect to SearchEngine");
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socket, &writeSet);
        timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &writeSet, nullptr, &timeout) <= 0) {
            throw std::runtime_error("SearchEngine connection timed out");
        }
        int socketError = 0;
        int errorSize = sizeof(socketError);
        getsockopt(
            socket, SOL_SOCKET, SO_ERROR,
            reinterpret_cast<char*>(&socketError), &errorSize);
        if (socketError != 0) {
            throw std::runtime_error("SearchEngine rejected the connection");
        }
    }
    nonBlocking = 0;
    ioctlsocket(socket, FIONBIO, &nonBlocking);
    const DWORD nativeTimeout = static_cast<DWORD>(timeoutMs);
    setsockopt(
        socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&nativeTimeout), sizeof(nativeTimeout));
    setsockopt(
        socket, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&nativeTimeout), sizeof(nativeTimeout));
}

void sendAll(SOCKET socket, const char* data, int size)
{
    int sent = 0;
    while (sent < size) {
        const int count = send(socket, data + sent, size - sent, 0);
        if (count <= 0) {
            throw std::runtime_error("cannot send SearchEngine PING");
        }
        sent += count;
    }
}

void receiveAll(SOCKET socket, char* data, int size)
{
    int received = 0;
    while (received < size) {
        const int count = recv(socket, data + received, size - received, 0);
        if (count <= 0) {
            throw std::runtime_error("SearchEngine PING response timed out");
        }
        received += count;
    }
}

int healthCommand(const std::vector<std::wstring>& args)
{
    const int port = parseInt(requiredOption(args, L"--port"), "port");
    int timeoutMs = 10000;
    if (const auto value = option(args, L"--timeout-ms")) {
        timeoutMs = parseInt(*value, "timeout");
    }
    if (port < 1 || port > 65535 || timeoutMs < 100 || timeoutMs > 60000) {
        throw std::runtime_error("invalid health-check port or timeout");
    }

    Winsock winsock;
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.get() == INVALID_SOCKET) {
        throw std::runtime_error("cannot create health-check socket");
    }
    const sockaddr_in endpoint = loopbackEndpoint(port);
    connectWithTimeout(socket.get(), endpoint, timeoutMs);

    struct Header {
        std::uint64_t size;
        std::uint64_t command;
    };
    static_assert(sizeof(Header) == 16, "SearchEngine wire header changed");
    const Header request{4, kPingCommand};
    const char ping[4] = {'P', 'I', 'N', 'G'};
    sendAll(socket.get(), reinterpret_cast<const char*>(&request), sizeof(request));
    sendAll(socket.get(), ping, sizeof(ping));

    Header response{};
    char pong[4]{};
    receiveAll(socket.get(), reinterpret_cast<char*>(&response), sizeof(response));
    if (response.size != sizeof(pong) || response.command != kPingCommand) {
        throw std::runtime_error("SearchEngine returned an unexpected PING header");
    }
    receiveAll(socket.get(), pong, sizeof(pong));
    if (std::string(pong, sizeof(pong)) != "PONG") {
        throw std::runtime_error("SearchEngine returned an invalid PING payload");
    }
    std::cout << "health_ok=1\nport=" << port << '\n';
    return 0;
}

void printUsage()
{
    std::cout
        << "SearchEngineConfig commands:\n"
        << "  system-info\n"
        << "  inspect --settings FILE\n"
        << "  validate --settings FILE [--check-dirs]\n"
        << "  format-json --settings FILE [--line-ending lf|crlf]\n"
        << "  compare-json --left FILE --right FILE\n"
        << "  validate-prefix-map --path FILE\n"
        << "  configure --template FILE --output FILE --port N --year N\n"
        << "            --threads N --file-timeout N --prm-autodetect 0|1\n"
        << "            [--import-settings FILE] [--parallel-readers N]\n"
        << "            [--sqlite-load-threads N]\n"
        << "            [--document-catalog-storage memory|sqlite]\n"
        << "            [--tlg-send-root PATH] [--razn-output-dir PATH]\n"
        << "            [--opis-base-dir PATH] [--f12-base-dir PATH]\n"
        << "  configure-interactive --template FILE --output FILE\n"
        << "            [--import-settings FILE] [--language auto|ru|en]\n"
        << "  choose-instance --default ID --output FILE\n"
        << "  choose-installed-instance --output FILE\n"
        << "            [--purpose uninstall|register-auth|configure]\n"
        << "  inspect-installed [--instance ID]\n"
        << "  check-port --port N\n"
        << "  health --port N [--timeout-ms N]\n"
        << "  backup --install-root DIR --data-dir DIR --destination DIR\n"
        << "         --mode full|settings-logs\n"
        << "  runtime-update-apply --data-dir DIR --package-data DIR\n"
        << "            --generated-settings FILE --generated-endpoint FILE\n"
        << "            --rollback-dir DIR\n"
        << "  runtime-update-rollback --data-dir DIR --rollback-dir DIR\n"
        << "  runtime-update-commit --data-dir DIR --rollback-dir DIR\n"
        << "  settings-transaction-apply --data-dir DIR --settings-temp FILE\n"
        << "            --rollback-dir DIR [--endpoint-temp FILE]\n"
        << "  settings-transaction-rollback --data-dir DIR --rollback-dir DIR\n"
        << "  settings-transaction-commit --data-dir DIR --rollback-dir DIR\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    try {
        if (argc < 2) {
            printUsage();
            return 1;
        }
        std::vector<std::wstring> args;
        for (int i = 2; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        const std::wstring command = argv[1];
        if (command == L"system-info") {
            printSystemInfo();
            return 0;
        }
        if (command == L"inspect") {
            return inspectCommand(args);
        }
        if (command == L"validate") {
            return validateCommand(args);
        }
        if (command == L"format-json") {
            return formatJsonCommand(args);
        }
        if (command == L"compare-json") {
            return compareJsonCommand(args);
        }
        if (command == L"validate-prefix-map") {
            return validatePrefixMapCommand(args);
        }
        if (command == L"configure") {
            return configureCommand(args);
        }
        if (command == L"configure-interactive") {
            return configureInteractiveCommand(args);
        }
        if (command == L"choose-instance") {
            return chooseInstanceCommand(args);
        }
        if (command == L"choose-installed-instance") {
            return chooseInstalledInstanceCommand(args);
        }
        if (command == L"inspect-installed") {
            return inspectInstalledCommand(args);
        }
        if (command == L"check-port") {
            return checkPortCommand(args);
        }
        if (command == L"health") {
            return healthCommand(args);
        }
        if (command == L"backup") {
            return backupCommand(args);
        }
        if (command == L"runtime-update-apply") {
            return runtime_data_transaction::applyCommand(args);
        }
        if (command == L"runtime-update-rollback") {
            return runtime_data_transaction::rollbackCommand(args);
        }
        if (command == L"runtime-update-commit") {
            return runtime_data_transaction::commitCommand(args);
        }
        if (command == L"settings-transaction-apply") {
            return runtime_data_transaction::settingsApplyCommand(args);
        }
        if (command == L"settings-transaction-rollback") {
            return runtime_data_transaction::settingsRollbackCommand(args);
        }
        if (command == L"settings-transaction-commit") {
            return runtime_data_transaction::settingsCommitCommand(args);
        }
        printUsage();
        return 1;
    } catch (const std::exception& exception) {
        std::cerr << "ERROR: " << exception.what() << '\n';
        return 1;
    }
}
