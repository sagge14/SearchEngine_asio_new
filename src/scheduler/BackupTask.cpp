#include "BackupTask.h"

#include "MyUtils/LogFile.h"

#include <exception>
#include <utility>

BackupTask::BackupTask(boost::asio::io_context& io,
                       boost::asio::any_io_executor cpu_ex,
                       std::chrono::seconds period,
                       fs::path backup_root,
                       const std::vector<BackupTarget>& targets)
    : AbstractScheduledTask(io, std::move(cpu_ex), period, true),
      engine_(std::move(backup_root), targets)
{
}

void BackupTask::runTask()
{
    try {
        engine_.runOnce();
    } catch (const std::exception& exception) {
        LogFile::getBackup().write(
            std::string("[ERROR] Backup task failed: ") + exception.what()
        );
    } catch (...) {
        LogFile::getBackup().write(
            "[ERROR] Backup task failed: unknown exception"
        );
    }
}
