#pragma once

#include "Backup/Restore/RestoreTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct MirrorManifestFile {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string captured_at;
    std::string method;
};

struct MirrorManifest {
    bool ok = false;
    bool complete = true;
    std::string source;
    std::int64_t updated_unix_seconds = 0;
    std::string updated_at;
    std::int64_t point_created_unix_seconds = 0;
    std::string point_created_at;
    std::string point_tier;
    std::vector<std::string> directories;
    std::vector<std::string> errors;
    std::vector<MirrorManifestFile> files;
    std::string error_message;
};

bool isSafeRelativeUtf8Path(const std::string& configured);

std::filesystem::path pathFromUtf8Path(const std::string& path);
std::string pathToUtf8Path(const std::filesystem::path& path);

std::filesystem::path mirrorObjectPath(
    const std::filesystem::path& target_root,
    const std::string& relative,
    const std::string& sha256
);

std::filesystem::path mirrorCurrentDataPath(
    const std::filesystem::path& target_root,
    const std::string& relative
);

MirrorManifest readMirrorManifest(
    const std::filesystem::path& manifest_path
);

bool resolveManifestFile(
    const std::filesystem::path& target_root,
    const MirrorManifestFile& file,
    RestoreResolveStatus& status,
    std::filesystem::path& resolved_path
);

void formatLocalDateTime(
    std::int64_t unix_seconds,
    std::string& date_local,
    std::string& time_local
);

std::string displayNameFromTargetId(const std::string& id);

bool looksLikeMirrorTargetRoot(const std::filesystem::path& path);
