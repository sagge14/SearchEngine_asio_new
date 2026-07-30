#include "BackupServiceOptions.h"

#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path absoluteFrom(const fs::path& base, const fs::path& value)
{
    if (value.is_absolute()) {
        return value.lexically_normal();
    }
    return (base / value).lexically_normal();
}

std::string narrowAscii(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        result.push_back(
            ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?'
        );
    }
    return result;
}

} // namespace

bool parseBackupServiceOptions(
    const std::vector<std::wstring>& arguments,
    BackupServiceOptions& options,
    std::string& error)
{
    bool console = false;
    bool once = false;
    bool service = false;

    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--console") {
            console = true;
        } else if (argument == L"--once") {
            once = true;
        } else if (argument == L"--service") {
            service = true;
        } else if (argument == L"--config" ||
                   argument == L"--data-dir" ||
                   argument == L"--service-name")
        {
            if (index + 1 >= arguments.size()) {
                error = narrowAscii(argument) + " requires a value";
                return false;
            }
            const std::wstring& value = arguments[++index];
            if (value.empty()) {
                error = narrowAscii(argument) + " must not be empty";
                return false;
            }
            if (argument == L"--config") {
                options.config = fs::path(value);
            } else if (argument == L"--data-dir") {
                options.data_dir = fs::path(value);
            } else {
                options.service_name = value;
            }
        } else {
            error = "unknown argument: " + narrowAscii(argument);
            return false;
        }
    }

    const int selected_modes =
        static_cast<int>(console) +
        static_cast<int>(once) +
        static_cast<int>(service);
    if (selected_modes > 1) {
        error = "--console, --once and --service are mutually exclusive";
        return false;
    }
    if (service && options.help) {
        error = "--service and --help are mutually exclusive";
        return false;
    }

    options.mode = service
        ? BackupLaunchMode::Service
        : (once ? BackupLaunchMode::Once : BackupLaunchMode::Console);
    return true;
}

BackupRuntimePaths resolveBackupRuntimePaths(
    const BackupServiceOptions& options,
    const fs::path& executable)
{
    BackupRuntimePaths result;

    std::error_code error;
    result.executable = fs::absolute(executable, error);
    if (error) {
        result.executable = executable;
    }
    result.executable = result.executable.lexically_normal();

    fs::path executable_dir = result.executable.parent_path();
    if (executable_dir.empty()) {
        executable_dir = fs::current_path(error);
        if (error) {
            executable_dir = fs::path{"."};
        }
    }

    result.data_dir = options.data_dir.empty()
        ? executable_dir
        : absoluteFrom(executable_dir, options.data_dir);
    result.config = options.config.empty()
        ? result.data_dir / "Backup.json"
        : absoluteFrom(result.data_dir, options.config);
    result.logs = result.data_dir / "logs";
    return result;
}

fs::path backupExecutablePath(const fs::path& argv0)
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length != 0 && length < buffer.size()) {
        buffer.resize(length);
        return fs::path(buffer);
    }
#endif
    std::error_code error;
    fs::path result = argv0.empty() ? fs::current_path(error) : argv0;
    const fs::path absolute = fs::absolute(result, error);
    return (error ? result : absolute).lexically_normal();
}

std::string backupServiceUsage()
{
    std::ostringstream stream;
    stream
        << "BackupService - independent snapshots and economical history\n"
        << "Usage:\n"
        << "  BackupService [--console] [--config <path>] [--data-dir <path>]\n"
        << "  BackupService --once [--config <path>] [--data-dir <path>]\n"
        << "  BackupService --service [--service-name <name>]"
           " [--config <path>] [--data-dir <path>]\n\n"
        << "Options:\n"
        << "  --console             Run the periodic scheduler in a console\n"
        << "  --once                Run every configured group once and exit\n"
        << "  --service             Run under the Windows Service Control Manager\n"
        << "  --config <path>       Config path; relative paths use data-dir\n"
        << "  --data-dir <path>     Runtime root; relative paths use the exe directory\n"
        << "  --service-name <name> SCM instance name\n"
        << "  --help, -h            Show this help\n";
    return stream.str();
}
