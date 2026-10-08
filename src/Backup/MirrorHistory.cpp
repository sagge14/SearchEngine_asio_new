#include "MirrorHistory.h"

#include "Backup/BackupCache.h"
#include "Backup/BackupPathFilter.h"
#include "Backup/FileHash.h"
#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace nh = nlohmann;

enum class EntryKind {
    Directory,
    File
};

struct HistoryFile {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string source_fingerprint;
    bool fingerprint_reliable = false;
    std::string captured_at;
    std::int64_t captured_unix_seconds = 0;
    std::string method;
};

struct HistoryState {
    bool exists = false;
    bool complete = false;
    std::map<std::string, HistoryFile> files;
    std::vector<std::string> directories;
    std::vector<std::string> errors;
};

std::atomic<unsigned long long> temporary_sequence{0};

std::string pathToUtf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

std::string systemErrorMessage(const std::error_code& error)
{
    return encoding::system_error_to_utf8(error.message());
}

fs::path pathFromUtf8(const std::string& path)
{
    return fs::path(encoding::utf8_to_wstring(path));
}

std::string relativeKey(const fs::path& path)
{
    return encoding::wstring_to_utf8(
        path.lexically_normal().generic_wstring()
    );
}

bool isSafeRelativePath(const std::string& configured)
{
    if (configured.empty()) {
        return false;
    }
    const fs::path path = pathFromUtf8(configured);
    if (path.empty() ||
        path.is_absolute() ||
        path.has_root_name() ||
        path.has_root_directory())
    {
        return false;
    }
    for (const fs::path& component : path) {
        if (component.empty() ||
            component == fs::path(".") ||
            component == fs::path(".."))
        {
            return false;
        }
    }
    return relativeKey(path) == configured;
}

std::int64_t unixSeconds(std::chrono::system_clock::time_point timestamp)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        timestamp.time_since_epoch()
    ).count();
}

void logLine(const std::string& line)
{
    LogFile::getBackup().write(line);
}

fs::path uniqueTemporaryPath(const fs::path& directory,
                             const std::string& prefix)
{
    for (;;) {
        const auto sequence = temporary_sequence.fetch_add(1);
        const fs::path candidate =
            directory / (prefix + std::to_string(sequence));
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

bool replaceFile(const fs::path& source,
                 const fs::path& destination,
                 std::string& error_message)
{
#ifdef _WIN32
    if (MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) != 0)
    {
        return true;
    }
    const DWORD code = GetLastError();
    error_message =
        "MoveFileExW failed with error " + std::to_string(code);
    return false;
#else
    std::error_code error;
    fs::rename(source, destination, error);
    if (!error) {
        return true;
    }
    error_message = systemErrorMessage(error);
    return false;
#endif
}

bool writeJsonAtomically(const fs::path& destination,
                         const nh::json& value,
                         std::string& error_message)
{
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        error_message =
            "cannot create manifest directory: " +
            systemErrorMessage(error);
        return false;
    }

    const fs::path temporary =
        uniqueTemporaryPath(destination.parent_path(), ".manifest.partial.");
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc
        );
        if (!output.is_open()) {
            error_message =
                "cannot create \"" + pathToUtf8(temporary) + '"';
            return false;
        }
        output
            << value.dump(
                2,
                ' ',
                false,
                nh::json::error_handler_t::replace
            )
            << '\n';
        output.flush();
        if (!output) {
            error_message =
                "cannot write \"" + pathToUtf8(temporary) + '"';
            output.close();
            fs::remove(temporary, error);
            return false;
        }
    }

    if (!replaceFile(temporary, destination, error_message)) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

nh::json fileToJson(const HistoryFile& file)
{
    return nh::json{
        {"path", file.path},
        {"size", file.size},
        {"sha256", file.sha256},
        {"source_fingerprint", file.source_fingerprint},
        {"fingerprint_reliable", file.fingerprint_reliable},
        {"captured_at", file.captured_at},
        {"captured_unix_seconds", file.captured_unix_seconds},
        {"method", file.method}
    };
}

nh::json stateToJson(const BackupTarget& target,
                     const HistoryState& state,
                     const std::string& time_string,
                     std::int64_t timestamp)
{
    nh::json files = nh::json::array();
    for (const auto& [key, file] : state.files) {
        (void)key;
        files.push_back(fileToJson(file));
    }

    return nh::json{
        {"format", 1},
        {"strategy", "mirror_history"},
        {"source", pathToUtf8(target.src)},
        {"updated_at", time_string},
        {"updated_unix_seconds", timestamp},
        {"complete", state.complete},
        {"exclude", target.exclude},
        {"directories", state.directories},
        {"files", std::move(files)},
        {"errors", state.errors}
    };
}

bool readState(const fs::path& manifest,
               HistoryState& state,
               std::string& error_message)
{
    std::error_code error;
    if (!fs::exists(manifest, error)) {
        if (error) {
            error_message =
                "cannot inspect current manifest: " +
                systemErrorMessage(error);
            return false;
        }
        return true;
    }

    std::ifstream input(manifest, std::ios::binary);
    if (!input.is_open()) {
        error_message =
            "cannot open current manifest \"" + pathToUtf8(manifest) + '"';
        return false;
    }

    nh::json root;
    try {
        input >> root;
        if (root.at("format").get<int>() != 1 ||
            root.at("strategy").get<std::string>() != "mirror_history" ||
            !root.at("files").is_array() ||
            !root.at("directories").is_array())
        {
            error_message = "unsupported current manifest format";
            return false;
        }

        state.exists = true;
        state.complete = root.value("complete", false);
        state.directories =
            root.at("directories").get<std::vector<std::string>>();
        if (!std::all_of(
                state.directories.begin(),
                state.directories.end(),
                isSafeRelativePath
            ))
        {
            error_message =
                "unsafe directory path in current manifest";
            return false;
        }
        state.errors =
            root.value("errors", std::vector<std::string>{});

        for (const auto& item : root.at("files")) {
            HistoryFile file;
            file.path = item.at("path").get<std::string>();
            file.size = item.at("size").get<std::uint64_t>();
            file.sha256 = item.at("sha256").get<std::string>();
            file.source_fingerprint =
                item.value("source_fingerprint", std::string{});
            file.fingerprint_reliable =
                item.value("fingerprint_reliable", false);
            file.captured_at =
                item.value("captured_at", std::string{});
            file.captured_unix_seconds =
                item.value("captured_unix_seconds", std::int64_t{0});
            file.method = item.value("method", std::string{});
            if (!isSafeRelativePath(file.path) ||
                file.sha256.size() != 64 ||
                !std::all_of(
                    file.sha256.begin(),
                    file.sha256.end(),
                    [](unsigned char ch) {
                        return std::isxdigit(ch) != 0;
                    }
                ) ||
                !state.files.emplace(file.path, std::move(file)).second)
            {
                error_message = "invalid file entry in current manifest";
                return false;
            }
        }
    } catch (const std::exception& exception) {
        error_message =
            "invalid current manifest: " + std::string(exception.what());
        return false;
    }
    return true;
}

bool isManagedSQLiteSidecar(const fs::path& path)
{
    if (!isSQLiteSidecar(path)) {
        return false;
    }

    std::string name = path.filename().string();
    std::transform(
        name.begin(),
        name.end(),
        name.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    constexpr std::array<const char*, 3> suffixes{
        "-journal",
        "-wal",
        "-shm"
    };
    for (const char* suffix : suffixes) {
        const std::string suffix_value = suffix;
        if (!name.ends_with(suffix_value) ||
            name.size() <= suffix_value.size())
        {
            continue;
        }

        const fs::path database =
            path.parent_path() /
            path.filename().string().substr(
                0,
                path.filename().string().size() - suffix_value.size()
            );
        std::error_code error;
        return
            fs::is_regular_file(database, error) &&
            !error &&
            isSQLiteDatabaseCandidate(database);
    }
    return false;
}

bool collectSourceEntries(const BackupTarget& target,
                          std::map<fs::path, EntryKind>& entries,
                          std::string& error_message)
{
    if (!target.is_directory) {
        entries.emplace(target.src.filename(), EntryKind::File);
        return true;
    }

    const BackupPathFilter filter(target.exclude);
    std::error_code error;
    for (fs::recursive_directory_iterator it(
             target.src,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            error_message =
                "cannot enumerate source: " + systemErrorMessage(error);
            return false;
        }

        std::error_code type_error;
        if (it->is_symlink(type_error)) {
            if (type_error) {
                error_message =
                    "cannot inspect \"" + pathToUtf8(it->path()) +
                    "\": " + systemErrorMessage(type_error);
                return false;
            }
            continue;
        }
        if (target.mode == BackupMode::Auto &&
            isManagedSQLiteSidecar(it->path()))
        {
            continue;
        }

        const fs::path relative =
            fs::relative(it->path(), target.src, error);
        if (error) {
            error_message =
                "cannot make relative path: " +
                systemErrorMessage(error);
            return false;
        }
        if (it->is_directory(type_error)) {
            if (filter.isDirectoryExcluded(relative)) {
                it.disable_recursion_pending();
                continue;
            }
            entries.emplace(relative, EntryKind::Directory);
        } else if (it->is_regular_file(type_error)) {
            if (filter.isFileExcluded(relative)) {
                continue;
            }
            entries.emplace(relative, EntryKind::File);
        } else if (type_error) {
            error_message =
                "cannot inspect \"" + pathToUtf8(it->path()) +
                "\": " + systemErrorMessage(type_error);
            return false;
        } else {
            error_message =
                "unsupported source entry \"" + pathToUtf8(it->path()) + '"';
            return false;
        }
    }
    return true;
}

BackupSourceState inspectFilesystemSource(const fs::path& source)
{
    const FileHashResult first = sha256File(source);
    if (!first.ok) {
        return BackupSourceState{
            false,
            false,
            false,
            {},
            first.message
        };
    }
    const FileHashResult second = sha256File(source);
    if (!second.ok) {
        return BackupSourceState{
            false,
            false,
            false,
            {},
            second.message
        };
    }
    if (first.size != second.size || first.sha256 != second.sha256) {
        return BackupSourceState{
            false,
            false,
            false,
            {},
            "source changed while it was inspected"
        };
    }
    return BackupSourceState{
        true,
        true,
        false,
        "sha256-v1:" + second.sha256,
        {}
    };
}

BackupCacheResult materializeFilesystem(
    const fs::path& source,
    const fs::path& destination)
{
    constexpr int max_attempts = 3;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const BackupSourceState before = inspectFilesystemSource(source);
        if (!before.ok) {
            return BackupCacheResult{false, {}, 0, before.message};
        }

        std::error_code error;
        fs::copy_file(
            source,
            destination,
            fs::copy_options::overwrite_existing,
            error
        );
        if (error) {
            return BackupCacheResult{
                false,
                {},
                0,
                "cannot copy source: " + systemErrorMessage(error)
            };
        }

        const BackupSourceState after = inspectFilesystemSource(source);
        const FileHashResult copied = sha256File(destination);
        if (after.ok &&
            copied.ok &&
            before.fingerprint == after.fingerprint &&
            before.fingerprint == "sha256-v1:" + copied.sha256)
        {
            return BackupCacheResult{
                true,
                "stable-file",
                copied.size,
                {},
                after.fingerprint,
                true
            };
        }
        fs::remove(destination, error);
    }
    return BackupCacheResult{
        false,
        {},
        0,
        "source changed during all copy attempts"
    };
}

fs::path objectPath(const fs::path& objects_root,
                    const std::string& relative,
                    const std::string& sha256)
{
    return objects_root / "by-path" / pathFromUtf8(relative) / sha256;
}

bool archiveCurrentFile(const fs::path& current_file,
                        const fs::path& objects_root,
                        const std::string& relative,
                        std::string& error_message,
                        fs::path* archived_object = nullptr)
{
    if (archived_object != nullptr) {
        archived_object->clear();
    }
    std::error_code error;
    if (!fs::exists(current_file, error)) {
        if (error) {
            error_message =
                "cannot inspect current file: " +
                systemErrorMessage(error);
            return false;
        }
        return true;
    }

    const FileHashResult current_hash = sha256File(current_file);
    if (!current_hash.ok) {
        error_message = current_hash.message;
        return false;
    }
    const fs::path object =
        objectPath(objects_root, relative, current_hash.sha256);
    if (archived_object != nullptr) {
        *archived_object = object;
    }
    fs::create_directories(object.parent_path(), error);
    if (error) {
        error_message =
            "cannot create object directory: " +
            systemErrorMessage(error);
        return false;
    }

    if (fs::exists(object, error)) {
        if (error) {
            error_message =
                "cannot inspect historical object: " +
                systemErrorMessage(error);
            return false;
        }
        fs::remove(current_file, error);
        if (error) {
            error_message =
                "cannot remove duplicate current file: " +
                systemErrorMessage(error);
            return false;
        }
        return true;
    }
    fs::rename(current_file, object, error);
    if (error) {
        error_message =
            "cannot archive current version: " +
            systemErrorMessage(error);
        return false;
    }
    return true;
}

bool publishCurrentFile(const fs::path& staged,
                        const fs::path& current_file,
                        const fs::path& objects_root,
                        const std::string& relative,
                        std::string& error_message)
{
    std::error_code error;
    fs::create_directories(current_file.parent_path(), error);
    if (error) {
        error_message =
            "cannot create current directory: " +
            systemErrorMessage(error);
        return false;
    }

    fs::path archived_object;
    if (!archiveCurrentFile(
            current_file,
            objects_root,
            relative,
            error_message,
            &archived_object
        ))
    {
        return false;
    }

    if (replaceFile(staged, current_file, error_message)) {
        return true;
    }

    // A publish failure must not leave the rolling mirror without the
    // previously valid version.
    if (!fs::exists(current_file, error) &&
        !error &&
        !archived_object.empty() &&
        fs::exists(archived_object, error) &&
        !error)
    {
        fs::copy_file(archived_object, current_file, error);
    }
    return false;
}

std::vector<fs::path> listPointDirectories(const fs::path& tier_root)
{
    std::vector<fs::path> points;
    std::error_code error;
    if (!fs::exists(tier_root, error) || error) {
        return points;
    }
    for (fs::directory_iterator it(
             tier_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        std::error_code type_error;
        if (it->is_directory(type_error) && !type_error) {
            points.push_back(it->path());
        }
    }
    std::sort(
        points.begin(),
        points.end(),
        [](const fs::path& lhs, const fs::path& rhs) {
            return lhs.filename().wstring() < rhs.filename().wstring();
        }
    );
    return points;
}

bool readPointTimestamp(const fs::path& point, std::int64_t& value)
{
    std::ifstream input(point / "manifest.json", std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    try {
        nh::json manifest;
        input >> manifest;
        value = manifest.at("point_created_unix_seconds").get<std::int64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

bool tierIsDue(const fs::path& tier_root,
               const BackupHistoryTier& tier,
               std::int64_t timestamp)
{
    const std::vector<fs::path> points = listPointDirectories(tier_root);
    if (points.empty()) {
        return true;
    }
    std::int64_t last_timestamp = 0;
    if (!readPointTimestamp(points.back(), last_timestamp)) {
        return true;
    }
    return
        timestamp < last_timestamp ||
        timestamp - last_timestamp >=
            static_cast<std::int64_t>(tier.period_sec);
}

bool createRestorePoint(const fs::path& restore_points_root,
                        const BackupHistoryTier& tier,
                        const std::string& time_string,
                        std::int64_t timestamp,
                        const nh::json& current_manifest,
                        fs::path& created_point,
                        std::string& error_message)
{
    const fs::path tier_root = restore_points_root / tier.name;
    if (!tierIsDue(tier_root, tier, timestamp)) {
        return true;
    }

    std::error_code error;
    fs::create_directories(tier_root, error);
    if (error) {
        error_message =
            "cannot create restore point tier: " +
            systemErrorMessage(error);
        return false;
    }

    fs::path point = tier_root / time_string;
    size_t suffix = 1;
    while (fs::exists(point, error) && !error) {
        point =
            tier_root / (time_string + "_" + std::to_string(suffix++));
    }
    if (error) {
        error_message =
            "cannot choose restore point name: " +
            systemErrorMessage(error);
        return false;
    }

    nh::json point_manifest = current_manifest;
    point_manifest["point_tier"] = tier.name;
    point_manifest["point_created_at"] = time_string;
    point_manifest["point_created_unix_seconds"] = timestamp;
    if (!writeJsonAtomically(
            point / "manifest.json",
            point_manifest,
            error_message
        ))
    {
        fs::remove_all(point, error);
        return false;
    }
    created_point = point;
    return true;
}

void rotateRestorePoints(const fs::path& restore_points_root,
                         const std::vector<BackupHistoryTier>& tiers)
{
    for (const auto& tier : tiers) {
        std::vector<fs::path> points =
            listPointDirectories(restore_points_root / tier.name);
        while (points.size() > tier.max_points) {
            std::error_code error;
            fs::remove_all(points.front(), error);
            if (error) {
                logLine(
                    "[WARNING] Cannot remove old restore point \"" +
                    pathToUtf8(points.front()) + "\": " +
                    systemErrorMessage(error)
                );
                break;
            }
            points.erase(points.begin());
        }
    }
}

using ObjectReference = std::pair<std::string, std::string>;

std::set<ObjectReference> collectPointReferences(
    const fs::path& restore_points_root)
{
    std::set<ObjectReference> references;
    std::error_code error;
    if (!fs::exists(restore_points_root, error) || error) {
        return references;
    }

    for (fs::recursive_directory_iterator it(
             restore_points_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        if (it->path().filename() != "manifest.json") {
            continue;
        }
        std::ifstream input(it->path(), std::ios::binary);
        if (!input.is_open()) {
            continue;
        }
        try {
            nh::json manifest;
            input >> manifest;
            for (const auto& file : manifest.at("files")) {
                references.emplace(
                    file.at("path").get<std::string>(),
                    file.at("sha256").get<std::string>()
                );
            }
        } catch (...) {
            logLine(
                "[WARNING] Cannot read restore point manifest \"" +
                pathToUtf8(it->path()) + "\" during object cleanup"
            );
        }
    }
    return references;
}

void collectGarbageObjects(const fs::path& objects_root,
                           const HistoryState& current,
                           const fs::path& restore_points_root)
{
    const fs::path by_path_root = objects_root / "by-path";
    const std::set<ObjectReference> references =
        collectPointReferences(restore_points_root);
    std::error_code error;
    if (!fs::exists(by_path_root, error) || error) {
        return;
    }

    std::vector<fs::path> directories;
    for (fs::recursive_directory_iterator it(
             by_path_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            if (!type_error) {
                directories.push_back(it->path());
            }
            continue;
        }
        if (!it->is_regular_file(type_error) || type_error) {
            continue;
        }

        const fs::path relative_object =
            fs::relative(it->path(), by_path_root, error);
        if (error || relative_object.empty()) {
            error.clear();
            continue;
        }
        const std::string hash = relative_object.filename().string();
        const fs::path relative_file =
            relative_object.parent_path();
        const std::string path = relativeKey(relative_file);
        const auto current_it = current.files.find(path);
        const bool is_current =
            current_it != current.files.end() &&
            current_it->second.sha256 == hash;
        if (is_current || !references.contains({path, hash})) {
            fs::remove(it->path(), error);
            if (error) {
                logLine(
                    "[WARNING] Cannot remove unused history object \"" +
                    pathToUtf8(it->path()) + "\": " +
                    systemErrorMessage(error)
                );
                error.clear();
            }
        }
    }

    std::sort(
        directories.begin(),
        directories.end(),
        [](const fs::path& lhs, const fs::path& rhs) {
            return
                std::distance(lhs.begin(), lhs.end()) >
                std::distance(rhs.begin(), rhs.end());
        }
    );
    for (const fs::path& directory : directories) {
        fs::remove(directory, error);
        error.clear();
    }
}

void removeEmptyCurrentDirectories(
    const fs::path& data_root,
    const std::vector<std::string>& keep_directories)
{
    std::set<std::string> keep;
    for (const std::string& directory : keep_directories) {
        keep.insert(directory);
    }

    std::vector<fs::path> directories;
    std::error_code error;
    if (!fs::exists(data_root, error) || error) {
        return;
    }
    for (fs::recursive_directory_iterator it(
             data_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        std::error_code type_error;
        if (it->is_directory(type_error) && !type_error) {
            directories.push_back(it->path());
        }
    }
    std::sort(
        directories.begin(),
        directories.end(),
        [](const fs::path& lhs, const fs::path& rhs) {
            return
                std::distance(lhs.begin(), lhs.end()) >
                std::distance(rhs.begin(), rhs.end());
        }
    );
    for (const fs::path& directory : directories) {
        const fs::path relative = fs::relative(directory, data_root, error);
        if (error) {
            error.clear();
            continue;
        }
        if (keep.contains(relativeKey(relative))) {
            continue;
        }
        fs::remove(directory, error);
        error.clear();
    }
}

} // namespace

BackupTargetResult updateMirrorHistory(
    const BackupTarget& target,
    const fs::path& target_root,
    const std::string& time_string,
    std::chrono::system_clock::time_point timestamp)
{
    const std::int64_t timestamp_seconds = unixSeconds(timestamp);
    const fs::path current_root = target_root / "current";
    const fs::path data_root = current_root / "data";
    const fs::path current_manifest_path = current_root / "manifest.json";
    const fs::path objects_root = target_root / "objects";
    const fs::path restore_points_root = target_root / "restore_points";
    const fs::path staging_root = target_root / ".staging";

    std::error_code error;
    fs::create_directories(data_root, error);
    if (error) {
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Failed,
            {},
            "cannot create current mirror: " + systemErrorMessage(error)
        };
    }
    fs::create_directories(staging_root, error);
    if (error) {
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Failed,
            {},
            "cannot create staging directory: " +
            systemErrorMessage(error)
        };
    }

    HistoryState state;
    std::string operation_error;
    if (!readState(current_manifest_path, state, operation_error)) {
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Failed,
            {},
            operation_error
        };
    }
    const bool state_existed = state.exists;
    const bool previous_complete = state.complete;
    const bool had_previous_errors = !state.errors.empty();
    state.errors.clear();

    std::map<fs::path, EntryKind> source_entries;
    if (!collectSourceEntries(target, source_entries, operation_error)) {
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Failed,
            {},
            operation_error
        };
    }

    std::set<std::string> source_files;
    std::vector<std::string> source_directories;
    bool data_changed = false;
    bool metadata_changed =
        !state_existed || !previous_complete || had_previous_errors;

    for (const auto& [relative_path, kind] : source_entries) {
        const std::string relative = relativeKey(relative_path);
        if (kind == EntryKind::Directory) {
            source_directories.push_back(relative);
            fs::create_directories(data_root / relative_path, error);
            if (error) {
                state.errors.push_back(
                    relative + ": cannot create current directory: " +
                    systemErrorMessage(error)
                );
                error.clear();
            }
            continue;
        }

        source_files.insert(relative);
        const fs::path source =
            target.is_directory ? target.src / relative_path : target.src;
        const fs::path current_file = data_root / relative_path;
        const BackupSourceState source_state =
            target.mode == BackupMode::Auto
                ? inspectBackupSource(source)
                : inspectFilesystemSource(source);
        if (!source_state.ok) {
            state.errors.push_back(
                relative + ": cannot inspect source: " +
                source_state.message
            );
            continue;
        }

        const auto previous = state.files.find(relative);
        bool current_file_present = fs::exists(current_file, error);
        if (error) {
            state.errors.push_back(
                relative + ": cannot inspect current copy: " +
                systemErrorMessage(error)
            );
            error.clear();
            continue;
        }
        const bool current_is_regular =
            current_file_present
                ? fs::is_regular_file(current_file, error)
                : false;
        if (error) {
            state.errors.push_back(
                relative + ": cannot inspect current copy type: " +
                systemErrorMessage(error)
            );
            error.clear();
            continue;
        }
        if (current_file_present && !current_is_regular) {
            state.errors.push_back(
                relative + ": current copy is not a regular file"
            );
            continue;
        }
        if (previous != state.files.end() &&
            current_file_present &&
            source_state.reliable &&
            previous->second.fingerprint_reliable &&
            source_state.fingerprint ==
                previous->second.source_fingerprint)
        {
            continue;
        }

        const fs::path staged =
            uniqueTemporaryPath(staging_root, "file.");
        BackupCacheResult materialized;
        if (target.mode == BackupMode::Auto) {
            BackupCache direct_cache(staging_root / "unused-cache", false);
            materialized =
                direct_cache.materialize(source, staged, relative_path);
        } else {
            materialized = materializeFilesystem(source, staged);
        }
        if (!materialized.ok) {
            state.errors.push_back(
                relative + ": cannot capture source: " +
                materialized.message
            );
            fs::remove(staged, error);
            error.clear();
            continue;
        }

        const FileHashResult staged_hash = sha256File(staged);
        if (!staged_hash.ok) {
            state.errors.push_back(
                relative + ": cannot hash captured file: " +
                staged_hash.message
            );
            fs::remove(staged, error);
            error.clear();
            continue;
        }

        bool same_content = false;
        if (current_file_present && previous != state.files.end() &&
            previous->second.sha256 == staged_hash.sha256)
        {
            const auto current_size = fs::file_size(current_file, error);
            same_content = !error && current_size == staged_hash.size;
            error.clear();
        }

        HistoryFile captured{
            relative,
            staged_hash.size,
            staged_hash.sha256,
            materialized.source_fingerprint,
            materialized.fingerprint_reliable,
            time_string,
            timestamp_seconds,
            materialized.method
        };
        if (same_content) {
            fs::remove(staged, error);
            error.clear();
            HistoryFile& existing = previous->second;
            if (existing.source_fingerprint !=
                    captured.source_fingerprint ||
                existing.fingerprint_reliable !=
                    captured.fingerprint_reliable ||
                existing.method != captured.method)
            {
                existing.source_fingerprint =
                    std::move(captured.source_fingerprint);
                existing.fingerprint_reliable =
                    captured.fingerprint_reliable;
                existing.method = std::move(captured.method);
                metadata_changed = true;
            }
            continue;
        }

        if (!publishCurrentFile(
                staged,
                current_file,
                objects_root,
                relative,
                operation_error
            ))
        {
            state.errors.push_back(
                relative + ": cannot publish current copy: " +
                operation_error
            );
            fs::remove(staged, error);
            error.clear();
            continue;
        }

        state.files[relative] = std::move(captured);
        data_changed = true;
        metadata_changed = true;
    }

    for (auto it = state.files.begin(); it != state.files.end();) {
        if (source_files.contains(it->first)) {
            ++it;
            continue;
        }
        const fs::path current_file =
            data_root / pathFromUtf8(it->first);
        if (!archiveCurrentFile(
                current_file,
                objects_root,
                it->first,
                operation_error
            ))
        {
            state.errors.push_back(
                it->first + ": cannot archive deleted file: " +
                operation_error
            );
            ++it;
            continue;
        }
        it = state.files.erase(it);
        data_changed = true;
        metadata_changed = true;
    }

    std::map<fs::path, EntryKind> source_entries_after;
    if (!collectSourceEntries(
            target,
            source_entries_after,
            operation_error
        ))
    {
        state.errors.push_back(
            "cannot verify source tree: " + operation_error
        );
    } else if (source_entries_after != source_entries) {
        state.errors.push_back(
            "source directory entries changed during the backup run"
        );
    }

    std::sort(source_directories.begin(), source_directories.end());
    if (state.directories != source_directories) {
        state.directories = std::move(source_directories);
        metadata_changed = true;
    }
    state.complete = state.errors.empty();
    if (state.complete != previous_complete) {
        metadata_changed = true;
    }

    const nh::json current_manifest =
        stateToJson(target, state, time_string, timestamp_seconds);
    if (metadata_changed || !state.complete || !state_existed) {
        if (!writeJsonAtomically(
                current_manifest_path,
                current_manifest,
                operation_error
            ))
        {
            return BackupTargetResult{
                target.src,
                BackupTargetStatus::Failed,
                {},
                operation_error
            };
        }
    }

    if (!state.complete) {
        removeEmptyCurrentDirectories(data_root, state.directories);
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Failed,
            current_root,
            std::to_string(state.errors.size()) +
                " file or source-tree error(s); current mirror kept, "
                "restore points were not created"
        };
    }

    bool point_created = false;
    fs::path newest_point;
    for (const auto& tier : target.history_tiers) {
        fs::path created;
        if (!createRestorePoint(
                restore_points_root,
                tier,
                time_string,
                timestamp_seconds,
                current_manifest,
                created,
                operation_error
            ))
        {
            return BackupTargetResult{
                target.src,
                BackupTargetStatus::Failed,
                current_root,
                operation_error
            };
        }
        if (!created.empty()) {
            point_created = true;
            newest_point = std::move(created);
        }
    }

    rotateRestorePoints(restore_points_root, target.history_tiers);
    collectGarbageObjects(objects_root, state, restore_points_root);
    removeEmptyCurrentDirectories(data_root, state.directories);

    if (!data_changed && !metadata_changed && !point_created) {
        return BackupTargetResult{
            target.src,
            BackupTargetStatus::Unchanged,
            current_root,
            "current mirror and restore points are unchanged"
        };
    }

    logLine(
        "Mirror history updated: " + pathToUtf8(target.src) +
        " -> " + pathToUtf8(current_root)
    );
    return BackupTargetResult{
        target.src,
        BackupTargetStatus::MirrorUpdated,
        point_created ? newest_point : current_root,
        point_created
            ? "current mirror updated; restore point created"
            : "current mirror updated"
    };
}
