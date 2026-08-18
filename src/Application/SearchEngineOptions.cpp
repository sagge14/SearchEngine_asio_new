#include "SearchEngineOptions.h"

#include <Windows.h>

#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

std::string narrowForError(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (size <= 0) {
        return "<unprintable>";
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );
    return result;
}

fs::path absoluteFrom(const fs::path& base, const fs::path& value)
{
    if (value.is_absolute()) {
        return value.lexically_normal();
    }
    return (base / value).lexically_normal();
}

} // namespace

bool parseSearchEngineOptions(
    const std::vector<std::wstring>& arguments,
    SearchEngineOptions& options,
    std::string& error)
{
    bool console = false;
    bool service = false;
    bool data_dir_seen = false;
    bool service_name_seen = false;

    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--console") {
            console = true;
        } else if (argument == L"--service") {
            service = true;
        } else if (argument == L"--service-name") {
            if (service_name_seen) {
                error = "--service-name may be specified only once";
                return false;
            }
            if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
                error = "--service-name requires a non-empty value";
                return false;
            }
            options.service_name = arguments[++index];
            service_name_seen = true;
        } else if (argument == L"--base-dir" || argument == L"--data-dir") {
            if (data_dir_seen) {
                error = "--base-dir and --data-dir may be specified only once";
                return false;
            }
            if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
                error = narrowForError(argument) + " requires a non-empty path";
                return false;
            }
            options.data_dir = fs::path(arguments[++index]);
            data_dir_seen = true;
        } else {
            error = "unknown argument: " + narrowForError(argument);
            return false;
        }
    }

    if (console && service) {
        error = "--console and --service are mutually exclusive";
        return false;
    }
    if (service && options.help) {
        error = "--service and --help are mutually exclusive";
        return false;
    }
    if (service_name_seen && !service) {
        error = "--service-name requires --service";
        return false;
    }
    if (options.service_name.size() > 256 ||
        options.service_name.find_first_of(L"/\\") != std::wstring::npos)
    {
        error = "--service-name must be at most 256 characters and contain no slash";
        return false;
    }

    options.mode = service
        ? SearchEngineLaunchMode::Service
        : SearchEngineLaunchMode::Console;
    return true;
}

std::filesystem::path searchEngineExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0 || length >= buffer.size()) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "GetModuleFileNameW failed"
        );
    }
    buffer.resize(length);
    return fs::path(buffer).lexically_normal();
}

SearchEngineRuntimePaths resolveSearchEngineRuntimePaths(
    const SearchEngineOptions& options,
    const fs::path& executable)
{
    SearchEngineRuntimePaths paths;
    std::error_code ec;
    paths.executable = fs::absolute(executable, ec);
    if (ec) {
        paths.executable = executable;
    }
    paths.executable = paths.executable.lexically_normal();

    fs::path executable_dir = paths.executable.parent_path();
    if (executable_dir.empty()) {
        executable_dir = fs::current_path(ec);
        if (ec) {
            executable_dir = fs::path{L"."};
        }
    }

    paths.data_dir = options.data_dir.empty()
        ? executable_dir
        : absoluteFrom(executable_dir, options.data_dir);
    paths.settings = paths.data_dir / L"Settings.json";
    paths.backup_settings = paths.data_dir / L"Backup.json";
    paths.oem866 = paths.data_dir / L"OEM866.INI";
    paths.index = paths.data_dir / L"inverted_index.sqlite";
    paths.log_database = paths.data_dir / L"log.db";
    paths.server_log = paths.data_dir / L"server_log.log";
    paths.logs = paths.data_dir / L"logs";
    paths.messages = paths.data_dir / L"messages";
    paths.auth_clients = paths.data_dir / L"auth_clients.sqlite";
    paths.prefix_map = paths.data_dir / L"prefix_map.json";
    return paths;
}

bool activateSearchEngineRuntimePaths(
    const SearchEngineRuntimePaths& paths,
    std::string& error)
{
    std::error_code ec;
    if (!fs::exists(paths.data_dir, ec) ||
        !fs::is_directory(paths.data_dir, ec))
    {
        error = "runtime data directory does not exist: " +
            paths.data_dir.string();
        return false;
    }

    fs::create_directories(paths.logs, ec);
    if (ec) {
        error = "cannot create logs directory: " + paths.logs.string() +
            ": " + ec.message();
        return false;
    }
    fs::create_directories(paths.messages, ec);
    if (ec) {
        error = "cannot create messages directory: " +
            paths.messages.string() + ": " + ec.message();
        return false;
    }

    if (!SetCurrentDirectoryW(paths.data_dir.c_str())) {
        error = "SetCurrentDirectoryW failed for " + paths.data_dir.string() +
            ": " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

std::string searchEngineUsage()
{
    std::ostringstream stream;
    stream
        << "SearchEngine ASIO server\n"
        << "Usage:\n"
        << "  SearchEngine.exe [--console] [--data-dir <absolute-path>]\n"
        << "  SearchEngine.exe --service [--service-name <name>] "
           "[--data-dir <absolute-path>]\n\n"
        << "Options:\n"
        << "  --console          Run interactively (default)\n"
        << "  --service          Connect to Windows Service Control Manager\n"
        << "  --service-name     SCM service name (default: SearchEngineService)\n"
        << "  --data-dir <path>  Runtime files root (defaults to exe directory)\n"
        << "  --base-dir <path>  Alias for --data-dir\n"
        << "  --help, -h         Show this help\n";
    return stream.str();
}
