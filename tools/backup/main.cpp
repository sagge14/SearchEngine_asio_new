#include "BackupServiceApplication.h"
#include "BackupServiceOptions.h"
#include "BackupWindowsService.h"

#include "Backup/SQLiteBackup.h"
#include "MyUtils/LogFile.h"

#include <boost/asio.hpp>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::wstring> commandLineArguments(
    int argc,
#ifdef _WIN32
    wchar_t* argv[]
#else
    char* argv[]
#endif
)
{
    std::vector<std::wstring> result;
    result.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
#ifdef _WIN32
        result.emplace_back(argv[index]);
#else
        const std::string argument(argv[index]);
        result.emplace_back(argument.begin(), argument.end());
#endif
    }
    return result;
}

int backupMain(
    int argc,
#ifdef _WIN32
    wchar_t* argv[]
#else
    char* argv[]
#endif
)
{
    const std::vector<std::wstring> arguments =
        commandLineArguments(argc, argv);

    BackupServiceOptions options;
    std::string option_error;
    if (!parseBackupServiceOptions(arguments, options, option_error)) {
        std::cerr << "ERROR: " << option_error << "\n\n"
                  << backupServiceUsage();
        return 2;
    }
    if (options.help) {
        std::cout << backupServiceUsage()
                  << "\nSQLite runtime: " << backupSQLiteVersion() << '\n';
        return 0;
    }

    const std::filesystem::path executable =
        backupExecutablePath(arguments.empty()
            ? std::filesystem::path{}
            : std::filesystem::path(arguments.front()));
    const BackupRuntimePaths paths =
        resolveBackupRuntimePaths(options, executable);
    LogFile::setLogsDirectory(paths.logs);
    LogFile::ensureLogsDir();

    if (options.mode == BackupLaunchMode::Service) {
        return runBackupWindowsService(options, paths);
    }

    BackupServiceApplication application;
    std::string error;
    if (!application.configure(options, paths, error)) {
        std::cerr << error << "\nBackupService was not started.\n";
        return 2;
    }

    if (options.mode == BackupLaunchMode::Once) {
        try {
            return runBackupOnce(application.groups(), true);
        } catch (const std::exception& exception) {
            const std::string message =
                std::string("BackupService fatal error: ") +
                exception.what();
            std::cerr << message << '\n';
            LogFile::getBackup().write("[ERROR] " + message);
            return 1;
        } catch (...) {
            const std::string message =
                "BackupService fatal unknown error";
            std::cerr << message << '\n';
            LogFile::getBackup().write("[ERROR] " + message);
            return 1;
        }
    }

    if (!application.start(error)) {
        std::cerr << error << '\n';
        return 1;
    }

    boost::asio::io_context signal_io;
    boost::asio::signal_set signals(signal_io, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code& signal_error, int signal_number) {
            if (signal_error) {
                return;
            }
            std::cout
                << "Stopping BackupService on signal "
                << signal_number
                << "...\n";
            application.requestStop();
        }
    );
    std::thread signal_thread([&signal_io]() {
        signal_io.run();
    });

    std::cout
        << "BackupService started with "
        << application.groups().size()
        << " job(s). Initial snapshots are running now.\n"
        << "Config: " << paths.config.string() << '\n'
        << "Logs: " << paths.logs.string() << '\n'
        << "Press Ctrl+C to stop.\n";

    application.wait();
    signal_io.stop();
    if (signal_thread.joinable()) {
        signal_thread.join();
    }
    return application.exitCode();
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    return backupMain(argc, argv);
}
#else
int main(int argc, char* argv[])
{
    return backupMain(argc, argv);
}
#endif
