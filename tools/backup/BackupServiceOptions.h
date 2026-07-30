#pragma once

#include <filesystem>
#include <string>
#include <vector>

enum class BackupLaunchMode {
    Console,
    Once,
    Service
};

struct BackupServiceOptions {
    BackupLaunchMode mode = BackupLaunchMode::Console;
    std::filesystem::path config;
    std::filesystem::path data_dir;
    std::wstring service_name = L"SearchEngineBackupService";
    bool help = false;
};

struct BackupRuntimePaths {
    std::filesystem::path executable;
    std::filesystem::path data_dir;
    std::filesystem::path config;
    std::filesystem::path logs;
};

bool parseBackupServiceOptions(
    const std::vector<std::wstring>& arguments,
    BackupServiceOptions& options,
    std::string& error
);

BackupRuntimePaths resolveBackupRuntimePaths(
    const BackupServiceOptions& options,
    const std::filesystem::path& executable
);

std::filesystem::path backupExecutablePath(
    const std::filesystem::path& argv0 = {}
);

std::string backupServiceUsage();
