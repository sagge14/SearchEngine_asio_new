#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class RestoreResolveStatus {
    InCurrent,
    InObjects,
    Missing
};

enum class RestorePlanSource {
    CurrentData,
    ObjectStore,
    Missing
};

struct RestoreTargetInfo {
    std::string id;
    std::string display_name;
    std::filesystem::path root_path;
    bool has_current = false;
    size_t file_count_current = 0;
    std::uint64_t total_size_current = 0;
};

struct RestorePointInfo {
    std::string target_id;
    std::string tier;
    std::string label;
    std::filesystem::path manifest_path;
    std::filesystem::path target_root;
    std::int64_t unix_seconds = 0;
    std::string date_local;
    std::string time_local;
    bool complete = true;
    bool is_current = false;
    size_t file_count = 0;
    std::uint64_t total_size = 0;
    size_t error_count = 0;
    std::string source_path;
};

struct RestoreFileInfo {
    std::string relative_path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string captured_at;
    std::string method;
    RestoreResolveStatus resolve_status = RestoreResolveStatus::Missing;
    std::filesystem::path resolved_path;
};

struct RestorePlanEntry {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    std::filesystem::path source_path;
    RestorePlanSource source = RestorePlanSource::Missing;
    std::string status;
};

struct RestoreRequest {
    RestorePointInfo point;
    std::filesystem::path destination;
    bool overwrite = false;
    std::vector<std::string> path_filter;
};
