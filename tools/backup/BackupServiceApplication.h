#pragma once

#include "Backup/BackupConfig.h"
#include "BackupServiceOptions.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BackupServiceApplication {
public:
    BackupServiceApplication();
    ~BackupServiceApplication();

    BackupServiceApplication(const BackupServiceApplication&) = delete;
    BackupServiceApplication& operator=(const BackupServiceApplication&) =
        delete;

    bool configure(
        const BackupServiceOptions& options,
        const BackupRuntimePaths& paths,
        std::string& error
    );

    bool start(std::string& error);
    void requestStop();
    void wait();
    bool waitFor(std::chrono::milliseconds timeout);
    void stop();

    int exitCode() const noexcept;
    bool isRunning() const noexcept;
    bool stopRequested() const noexcept;
    const BackupRuntimePaths& paths() const noexcept;
    const std::vector<BackupGroup>& groups() const noexcept;

private:
    struct Runtime;

    void finishRuntime();
    void resolveConfiguredPaths();

    BackupServiceOptions options_;
    BackupRuntimePaths paths_;
    std::vector<BackupGroup> groups_;
    std::unique_ptr<Runtime> runtime_;
    std::thread runtime_thread_;

    std::atomic<bool> configured_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> exit_code_{0};
    mutable std::mutex state_mutex_;
    std::condition_variable finished_condition_;
};

int runBackupOnce(
    const std::vector<BackupGroup>& groups,
    bool print_results
);

// Parse Backup.json, resolve paths, reject mapped drives. Returns 0 or 2.
int validateBackupServiceConfig(const BackupRuntimePaths& paths);
