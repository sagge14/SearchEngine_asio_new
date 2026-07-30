#include "BackupServiceApplication.h"

#include "Backup/BackupEngine.h"
#include "MyUtils/LogFile.h"
#include "scheduler/BackupTask.h"
#include "scheduler/PeriodicTaskManager.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

namespace fs = std::filesystem;

enum class BackupTaskId {
    Group
};

size_t workerCount(size_t group_count)
{
    const size_t hardware = std::max(
        size_t(1),
        static_cast<size_t>(std::thread::hardware_concurrency())
    );
    return std::max(size_t(1), std::min(group_count, hardware));
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

std::string configErrorText(const BackupConfigResult& config)
{
    std::ostringstream stream;
    for (size_t index = 0; index < config.issues.size(); ++index) {
        if (index != 0) {
            stream << '\n';
        }
        stream
            << "Backup config error at "
            << config.issues[index].location
            << ": "
            << config.issues[index].message;
    }
    return stream.str();
}

} // namespace

struct BackupServiceApplication::Runtime {
    using WorkGuard = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type
    >;

    explicit Runtime(size_t worker_count)
        : guard(boost::asio::make_work_guard(scheduler_io)),
          workers(worker_count)
    {
    }

    boost::asio::io_context scheduler_io;
    WorkGuard guard;
    boost::asio::thread_pool workers;
    PeriodicTaskManager<BackupTaskId> scheduler;
};

BackupServiceApplication::BackupServiceApplication() = default;

BackupServiceApplication::~BackupServiceApplication()
{
    stop();
}

bool BackupServiceApplication::configure(
    const BackupServiceOptions& options,
    const BackupRuntimePaths& paths,
    std::string& error)
{
    if (running_.load(std::memory_order_acquire)) {
        error = "BackupServiceApplication is already running";
        return false;
    }
    if (runtime_) {
        error = "BackupServiceApplication cannot be reconfigured after start";
        return false;
    }

    configured_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    groups_.clear();
    options_ = options;
    paths_ = paths;
    LogFile::setLogsDirectory(paths_.logs);
    LogFile::ensureLogsDir();

    const BackupConfigResult config = loadBackupConfig(paths_.config);
    if (!config.ok()) {
        error = configErrorText(config);
        LogFile::getBackup().write("[CONFIG ERROR] " + error);
        return false;
    }
    if (config.groups.empty()) {
        error = "BackupService was not started: no backup jobs";
        LogFile::getBackup().write("[CONFIG ERROR] " + error);
        return false;
    }

    groups_ = config.groups;
    resolveConfiguredPaths();
    configured_.store(true, std::memory_order_release);
    exit_code_.store(0, std::memory_order_relaxed);
    return true;
}

void BackupServiceApplication::resolveConfiguredPaths()
{
    fs::path config_dir = paths_.config.parent_path();
    for (auto& group : groups_) {
        fs::path backup_dir(group.backup_dir);
        if (backup_dir.is_relative()) {
            backup_dir = config_dir / backup_dir;
        }
        group.backup_dir = backup_dir.lexically_normal().string();
        for (auto& target : group.targets) {
            if (target.src.is_relative()) {
                target.src = (config_dir / target.src).lexically_normal();
            } else {
                target.src = target.src.lexically_normal();
            }
        }
    }
}

bool BackupServiceApplication::start(std::string& error)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!configured_.load(std::memory_order_acquire)) {
        error = "BackupServiceApplication is not configured";
        return false;
    }
    if (runtime_) {
        error = "BackupServiceApplication has already been started";
        return false;
    }

    try {
        runtime_ = std::make_unique<Runtime>(workerCount(groups_.size()));
        for (const auto& group : groups_) {
            runtime_->scheduler.addTask<BackupTask>(
                BackupTaskId::Group,
                runtime_->scheduler_io,
                runtime_->workers.get_executor(),
                std::chrono::seconds(group.period_sec),
                group.backup_dir,
                group.targets
            );
        }

        finished_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        runtime_thread_ = std::thread([this]() {
            finishRuntime();
        });

        if (stop_requested_.load(std::memory_order_acquire)) {
            boost::asio::post(runtime_->scheduler_io, [this]() {
                runtime_->scheduler.stopAll();
                runtime_->guard.reset();
                runtime_->scheduler_io.stop();
            });
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("cannot start backup scheduler: ") +
            exception.what();
    } catch (...) {
        error = "cannot start backup scheduler: unknown exception";
    }

    if (runtime_) {
        runtime_->scheduler.stopAll();
        runtime_->guard.reset();
        runtime_->scheduler_io.stop();
        runtime_->workers.join();
        runtime_.reset();
    }
    running_.store(false, std::memory_order_release);
    exit_code_.store(1, std::memory_order_relaxed);
    LogFile::getBackup().write("[ERROR] " + error);
    return false;
}

void BackupServiceApplication::requestStop()
{
    const bool first_request =
        !stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (!first_request) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!runtime_) {
        return;
    }

    boost::asio::post(runtime_->scheduler_io, [this]() {
        runtime_->scheduler.stopAll();
        runtime_->guard.reset();
        runtime_->scheduler_io.stop();
    });
}

void BackupServiceApplication::finishRuntime()
{
    try {
        runtime_->scheduler_io.run();
    } catch (const std::exception& exception) {
        exit_code_.store(1, std::memory_order_relaxed);
        LogFile::getBackup().write(
            std::string("[ERROR] Backup scheduler failed: ") +
            exception.what()
        );
    } catch (...) {
        exit_code_.store(1, std::memory_order_relaxed);
        LogFile::getBackup().write(
            "[ERROR] Backup scheduler failed: unknown exception"
        );
    }

    runtime_->scheduler.stopAll();
    runtime_->guard.reset();
    runtime_->scheduler_io.stop();
    runtime_->workers.join();

    running_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    finished_condition_.notify_all();
}

void BackupServiceApplication::wait()
{
    if (runtime_thread_.joinable()) {
        runtime_thread_.join();
    }
}

bool BackupServiceApplication::waitFor(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    return finished_condition_.wait_for(lock, timeout, [this]() {
        return finished_.load(std::memory_order_acquire);
    });
}

void BackupServiceApplication::stop()
{
    requestStop();
    wait();
}

int BackupServiceApplication::exitCode() const noexcept
{
    return exit_code_.load(std::memory_order_relaxed);
}

bool BackupServiceApplication::isRunning() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

bool BackupServiceApplication::stopRequested() const noexcept
{
    return stop_requested_.load(std::memory_order_acquire);
}

const BackupRuntimePaths& BackupServiceApplication::paths() const noexcept
{
    return paths_;
}

const std::vector<BackupGroup>&
BackupServiceApplication::groups() const noexcept
{
    return groups_;
}

int runBackupOnce(
    const std::vector<BackupGroup>& groups,
    bool print_results)
{
    bool success = true;
    for (const auto& group : groups) {
        BackupEngine engine(group.backup_dir, group.targets);
        const BackupRunResult result = engine.runOnce();
        success = success && result.ok();

        if (!print_results) {
            continue;
        }
        for (const auto& target : result.targets) {
            std::cout << statusName(target.status) << ": "
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
