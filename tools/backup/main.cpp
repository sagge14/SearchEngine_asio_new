#include "Backup/BackupConfig.h"
#include "Backup/BackupEngine.h"
#include "Backup/SQLiteBackup.h"
#include "MyUtils/LogFile.h"
#include "scheduler/BackupTask.h"
#include "scheduler/PeriodicTaskManager.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Options {
    fs::path config = "Backup.json";
    bool once = false;
    bool help = false;
};

enum class BackupTaskId {
    Group
};

void printUsage()
{
    std::cout
        << "BackupService - independent snapshots and economical history\n"
        << "SQLite runtime: " << backupSQLiteVersion() << "\n\n"
        << "Usage:\n"
        << "  BackupService [--config <Backup.json>] [--once]\n\n"
        << "Options:\n"
        << "  --config <path>  Configuration file (default: Backup.json)\n"
        << "  --once           Run every valid job once and exit\n"
        << "  --help, -h       Show this help\n";
}

bool parseOptions(int argc,
                  char* argv[],
                  Options& options,
                  std::string& error)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--config") {
            if (index + 1 >= argc) {
                error = "--config requires a path";
                return false;
            }
            options.config = argv[++index];
        } else {
            error = "unknown argument: " + argument;
            return false;
        }
    }
    return true;
}

const char* statusName(BackupTargetStatus status)
{
    switch (status) {
    case BackupTargetStatus::SnapshotCreated:
        return "created";
    case BackupTargetStatus::MirrorUpdated:
        return "mirror-updated";
    case BackupTargetStatus::Unchanged:
        return "unchanged";
    case BackupTargetStatus::Failed:
        return "failed";
    }
    return "unknown";
}

void printConfigIssues(const BackupConfigResult& config)
{
    for (const auto& issue : config.issues) {
        const std::string message =
            "Backup config error at " + issue.location + ": " + issue.message;
        std::cerr << message << '\n';
        LogFile::getBackup().write("[CONFIG ERROR] " + message);
    }
}

int runOnce(const std::vector<BackupGroup>& groups)
{
    bool success = true;
    for (const auto& group : groups) {
        BackupEngine engine(group.backup_dir, group.targets);
        const BackupRunResult result = engine.runOnce();
        success = success && result.ok();

        for (const auto& target : result.targets) {
            std::cout
                << statusName(target.status)
                << ": "
                << target.source.string();
            if (!target.snapshot.empty()) {
                std::cout << " -> " << target.snapshot.string();
            }
            if (!target.message.empty()) {
                std::cout << " (" << target.message << ')';
            }
            std::cout << '\n';
        }
    }
    return success ? 0 : 1;
}

size_t workerCount(size_t group_count)
{
    const size_t hardware = std::max(
        size_t(1),
        static_cast<size_t>(std::thread::hardware_concurrency())
    );
    return std::max(size_t(1), std::min(group_count, hardware));
}

int runPeriodic(const std::vector<BackupGroup>& groups)
{
    boost::asio::io_context scheduler_io;
    auto work_guard = boost::asio::make_work_guard(scheduler_io);
    boost::asio::thread_pool workers(workerCount(groups.size()));
    PeriodicTaskManager<BackupTaskId> scheduler;

    for (const auto& group : groups) {
        scheduler.addTask<BackupTask>(
            BackupTaskId::Group,
            scheduler_io,
            workers.get_executor(),
            std::chrono::seconds(group.period_sec),
            group.backup_dir,
            group.targets
        );
    }

    boost::asio::signal_set signals(scheduler_io, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code& error, int signal_number) {
            if (error) {
                return;
            }
            std::cout
                << "Stopping BackupService on signal "
                << signal_number
                << "...\n";
            scheduler.stopAll();
            work_guard.reset();
            scheduler_io.stop();
        }
    );

    std::cout
        << "BackupService started with "
        << groups.size()
        << " job(s). Initial snapshots are running now.\n"
        << "Press Ctrl+C to stop.\n";

    scheduler_io.run();
    scheduler.stopAll();
    workers.join();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    LogFile::ensureLogsDir();

    Options options;
    std::string option_error;
    if (!parseOptions(argc, argv, options, option_error)) {
        std::cerr << "ERROR: " << option_error << "\n\n";
        printUsage();
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }

    const BackupConfigResult config = loadBackupConfig(options.config);
    if (!config.ok()) {
        printConfigIssues(config);
        std::cerr << "BackupService was not started.\n";
        return 2;
    }
    if (config.groups.empty()) {
        std::cerr << "BackupService was not started: no backup jobs.\n";
        return 2;
    }

    try {
        return options.once
            ? runOnce(config.groups)
            : runPeriodic(config.groups);
    } catch (const std::exception& exception) {
        const std::string message =
            std::string("BackupService fatal error: ") + exception.what();
        std::cerr << message << '\n';
        LogFile::getBackup().write("[ERROR] " + message);
        return 1;
    } catch (...) {
        const std::string message = "BackupService fatal unknown error";
        std::cerr << message << '\n';
        LogFile::getBackup().write("[ERROR] " + message);
        return 1;
    }
}
