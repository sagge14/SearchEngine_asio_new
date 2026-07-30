#pragma once

#include <chrono>
#include <filesystem>
#include <vector>

#include "AbstractScheduledTask.h"
#include "Backup/BackupEngine.h"

class BackupTask : public AbstractScheduledTask {
public:
    BackupTask(boost::asio::io_context& io,
               boost::asio::any_io_executor cpu_ex,
               std::chrono::seconds period,
               fs::path backup_root,
               const std::vector<BackupTarget>& targets);

    void runTask() override;

private:
    BackupEngine engine_;
};
