#pragma once

#include "BackupEngine.h"

#include <chrono>
#include <filesystem>
#include <string>

BackupTargetResult updateMirrorHistory(
    const BackupTarget& target,
    const std::filesystem::path& target_root,
    const std::string& time_string,
    std::chrono::system_clock::time_point timestamp
);
