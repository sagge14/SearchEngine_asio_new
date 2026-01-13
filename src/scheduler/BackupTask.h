#pragma once
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include "AbstractScheduledTask.h" // твой базовый класс задачи

namespace fs = std::filesystem;

struct BackupTarget {
    fs::path src;
    size_t max_versions = 5;
    bool is_directory;
};

struct BackupGroup {
    std::string backup_dir;
    size_t period_sec = 3600;
    std::vector<BackupTarget> targets;
};

class BackupTask : public AbstractScheduledTask {
    fs::path backup_root_;
    std::vector<BackupTarget> targets_;
public:
    BackupTask(boost::asio::io_context& io, std::chrono::seconds period,
               const fs::path& backup_root, const std::vector<BackupTarget>& targets);

    void runTask() override;

private:
    std::string getTimeString(std::chrono::system_clock::time_point tp);
    void backupDirectory(const BackupTarget& target, const std::string& time_str);
    void backupFile(const BackupTarget& target, const std::string& time_str);
    void cleanupOldBackups(const BackupTarget& target, bool is_dir);
};
