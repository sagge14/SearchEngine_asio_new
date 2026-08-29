#pragma once

#include <filesystem>
#include <string>
#include <vector>

enum class SearchEngineLaunchMode {
    Console,
    Service,
    InitialUpdate
};

struct SearchEngineOptions {
    SearchEngineLaunchMode mode = SearchEngineLaunchMode::Console;
    std::filesystem::path data_dir;
    std::wstring service_name = L"SearchEngineService";
    bool help = false;
};

struct SearchEngineRuntimePaths {
    std::filesystem::path executable;
    std::filesystem::path data_dir;
    std::filesystem::path settings;
    std::filesystem::path backup_settings;
    std::filesystem::path oem866;
    std::filesystem::path index;
    std::filesystem::path log_database;
    std::filesystem::path server_log;
    std::filesystem::path logs;
    std::filesystem::path messages;
    std::filesystem::path auth_clients;
    std::filesystem::path prefix_map;
};

bool parseSearchEngineOptions(
    const std::vector<std::wstring>& arguments,
    SearchEngineOptions& options,
    std::string& error
);

std::filesystem::path searchEngineExecutablePath();

SearchEngineRuntimePaths resolveSearchEngineRuntimePaths(
    const SearchEngineOptions& options,
    const std::filesystem::path& executable
);

bool activateSearchEngineRuntimePaths(
    const SearchEngineRuntimePaths& paths,
    std::string& error
);

std::string searchEngineUsage();
