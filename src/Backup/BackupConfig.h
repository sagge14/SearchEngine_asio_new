#pragma once

#include "BackupEngine.h"

#include <filesystem>
#include <string>
#include <vector>

struct BackupConfigIssue {
    std::string location;
    std::string message;
};

struct BackupConfigResult {
    std::vector<BackupGroup> groups;
    std::vector<BackupConfigIssue> issues;

    bool ok() const noexcept
    {
        return issues.empty();
    }
};

BackupConfigResult loadBackupConfig(
    const std::filesystem::path& path = "Backup.json"
);
