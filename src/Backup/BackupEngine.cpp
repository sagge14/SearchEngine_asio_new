#include "BackupEngine.h"

#include "Backup/BackupCache.h"
#include "Backup/MirrorHistory.h"
#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace {

enum class CompareResult {
    Equal,
    Different,
    Error
};

enum class TreeEntryKind {
    Directory,
    File
};

struct SnapshotFileRecord {
    fs::path relative_path;
    std::string method;
    std::uint64_t size = 0;
    bool sqlite = false;
    std::string source_fingerprint;
    bool fingerprint_reliable = false;
};

std::atomic<unsigned long long> staging_sequence{0};

std::string pathToUtf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

void logLine(const std::string& line)
{
    LogFile::getBackup().write(line);
}

void logError(const char* operation,
              const fs::path& path,
              const std::error_code& error)
{
    logLine(
        std::string("[ERROR] ") + operation + " \"" + pathToUtf8(path) +
        "\": (" + std::to_string(error.value()) + ") " + error.message()
    );
}

void logError(const char* operation,
              const fs::path& path,
              const std::string& message)
{
    logLine(
        std::string("[ERROR] ") + operation + " \"" + pathToUtf8(path) +
        "\": " + message
    );
}

void logWarning(const char* operation,
                const fs::path& path,
                const std::string& message)
{
    logLine(
        std::string("[WARNING] ") + operation + " \"" + pathToUtf8(path) +
        "\": " + message
    );
}

fs::path normalizedAbsolute(const fs::path& path, std::error_code& error)
{
    fs::path absolute = fs::absolute(path, error);
    if (error) {
        return {};
    }
    return absolute.lexically_normal();
}

bool componentsEqual(const fs::path& lhs, const fs::path& rhs)
{
#ifdef _WIN32
    std::wstring lhs_value = lhs.wstring();
    std::wstring rhs_value = rhs.wstring();
    std::transform(
        lhs_value.begin(),
        lhs_value.end(),
        lhs_value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); }
    );
    std::transform(
        rhs_value.begin(),
        rhs_value.end(),
        rhs_value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); }
    );
    return lhs_value == rhs_value;
#else
    return lhs == rhs;
#endif
}

bool pathIsWithin(const fs::path& candidate, const fs::path& parent)
{
    auto candidate_it = candidate.begin();
    auto parent_it = parent.begin();

    for (; parent_it != parent.end(); ++parent_it, ++candidate_it) {
        if (candidate_it == candidate.end() ||
            !componentsEqual(*candidate_it, *parent_it))
        {
            return false;
        }
    }
    return true;
}

std::string normalizedPathKey(const fs::path& path)
{
    std::wstring value = path.lexically_normal().generic_wstring();
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); }
    );
#endif
    return encoding::wstring_to_utf8(value);
}

std::uint64_t fnv1a64(const std::string& value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string safeTargetName(const fs::path& source)
{
    std::string name = pathToUtf8(source.filename());
    if (name.empty()) {
        name = "target";
    }

    for (char& ch : name) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '.' && ch != '-' && ch != '_') {
            ch = '_';
        }
    }

    constexpr size_t max_name_length = 48;
    if (name.size() > max_name_length) {
        name.resize(max_name_length);
    }
    return name;
}

std::string targetDirectoryName(const fs::path& normalized_source)
{
    std::ostringstream stream;
    stream
        << safeTargetName(normalized_source)
        << '_'
        << std::hex
        << std::setfill('0')
        << std::setw(16)
        << fnv1a64(normalizedPathKey(normalized_source));
    return stream.str();
}

bool isSnapshotName(const std::string& name)
{
    if (name.size() < 15 || name[8] != '_') {
        return false;
    }

    for (size_t index = 0; index < 15; ++index) {
        if (index == 8) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(name[index]))) {
            return false;
        }
    }

    if (name.size() == 15) {
        return true;
    }
    if (name[15] != '_' || name.size() == 16) {
        return false;
    }
    return std::all_of(
        name.begin() + 16,
        name.end(),
        [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        }
    );
}

std::vector<fs::path> listSnapshots(const fs::path& snapshots_root,
                                    std::error_code& error)
{
    std::vector<fs::path> snapshots;
    if (!fs::exists(snapshots_root, error)) {
        return snapshots;
    }
    if (error) {
        return snapshots;
    }

    for (fs::directory_iterator it(
             snapshots_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            return {};
        }

        std::error_code type_error;
        if (!it->is_directory(type_error)) {
            if (type_error) {
                error = type_error;
                return {};
            }
            continue;
        }

        const std::string name = it->path().filename().string();
        if (isSnapshotName(name)) {
            snapshots.push_back(it->path());
        }
    }

    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [](const fs::path& lhs, const fs::path& rhs) {
            return lhs.filename().string() < rhs.filename().string();
        }
    );
    return snapshots;
}

CompareResult compareFiles(const fs::path& lhs,
                           const fs::path& rhs,
                           std::string& error_message)
{
    std::error_code error;
    const auto lhs_size = fs::file_size(lhs, error);
    if (error) {
        error_message =
            "cannot read size of \"" + pathToUtf8(lhs) + "\": " +
            error.message();
        return CompareResult::Error;
    }
    const auto rhs_size = fs::file_size(rhs, error);
    if (error) {
        error_message =
            "cannot read size of \"" + pathToUtf8(rhs) + "\": " +
            error.message();
        return CompareResult::Error;
    }
    if (lhs_size != rhs_size) {
        return CompareResult::Different;
    }

    std::ifstream lhs_stream(lhs, std::ios::binary);
    std::ifstream rhs_stream(rhs, std::ios::binary);
    if (!lhs_stream.is_open() || !rhs_stream.is_open()) {
        error_message =
            "cannot open files for comparison: \"" + pathToUtf8(lhs) +
            "\" and \"" + pathToUtf8(rhs) + '"';
        return CompareResult::Error;
    }

    std::array<char, 64 * 1024> lhs_buffer{};
    std::array<char, 64 * 1024> rhs_buffer{};
    while (lhs_stream || rhs_stream) {
        lhs_stream.read(
            lhs_buffer.data(),
            static_cast<std::streamsize>(lhs_buffer.size())
        );
        rhs_stream.read(
            rhs_buffer.data(),
            static_cast<std::streamsize>(rhs_buffer.size())
        );

        const auto lhs_count = lhs_stream.gcount();
        const auto rhs_count = rhs_stream.gcount();
        if (lhs_count != rhs_count ||
            !std::equal(
                lhs_buffer.begin(),
                lhs_buffer.begin() + lhs_count,
                rhs_buffer.begin()
            ))
        {
            return CompareResult::Different;
        }
    }

    if (lhs_stream.bad() || rhs_stream.bad()) {
        error_message =
            "I/O error while comparing \"" + pathToUtf8(lhs) + "\" and \"" +
            pathToUtf8(rhs) + '"';
        return CompareResult::Error;
    }
    return CompareResult::Equal;
}

CompareResult collectTree(const fs::path& root,
                          std::map<fs::path, TreeEntryKind>& entries,
                          std::string& error_message)
{
    std::error_code error;
    for (fs::recursive_directory_iterator it(
             root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            error_message =
                "cannot enumerate \"" + pathToUtf8(root) + "\": " +
                error.message();
            return CompareResult::Error;
        }

        std::error_code type_error;
        if (it->is_symlink(type_error)) {
            if (type_error) {
                error_message =
                    "cannot inspect \"" + pathToUtf8(it->path()) + "\": " +
                    type_error.message();
                return CompareResult::Error;
            }
            continue;
        }

        const fs::path relative = fs::relative(it->path(), root, error);
        if (error) {
            error_message =
                "cannot make relative path for \"" + pathToUtf8(it->path()) +
                "\": " + error.message();
            return CompareResult::Error;
        }

        if (it->is_directory(type_error)) {
            entries.emplace(relative, TreeEntryKind::Directory);
        } else if (it->is_regular_file(type_error)) {
            entries.emplace(relative, TreeEntryKind::File);
        } else if (type_error) {
            error_message =
                "cannot inspect \"" + pathToUtf8(it->path()) + "\": " +
                type_error.message();
            return CompareResult::Error;
        } else {
            error_message =
                "unsupported filesystem entry \"" + pathToUtf8(it->path()) +
                '"';
            return CompareResult::Error;
        }
    }
    return CompareResult::Equal;
}

CompareResult compareDirectories(const fs::path& lhs,
                                 const fs::path& rhs,
                                 std::string& error_message)
{
    std::map<fs::path, TreeEntryKind> lhs_entries;
    std::map<fs::path, TreeEntryKind> rhs_entries;

    if (collectTree(lhs, lhs_entries, error_message) == CompareResult::Error ||
        collectTree(rhs, rhs_entries, error_message) == CompareResult::Error)
    {
        return CompareResult::Error;
    }
    if (lhs_entries != rhs_entries) {
        return CompareResult::Different;
    }

    for (const auto& [relative, kind] : lhs_entries) {
        if (kind != TreeEntryKind::File) {
            continue;
        }
        const CompareResult result =
            compareFiles(lhs / relative, rhs / relative, error_message);
        if (result != CompareResult::Equal) {
            return result;
        }
    }
    return CompareResult::Equal;
}

CompareResult compareSourceToSnapshot(const BackupTarget& target,
                                      const fs::path& snapshot_data,
                                      std::string& error_message)
{
    if (target.is_directory) {
        return compareDirectories(target.src, snapshot_data, error_message);
    }
    return compareFiles(
        target.src,
        snapshot_data / target.src.filename(),
        error_message
    );
}

bool isManagedSQLiteSidecar(const fs::path& path)
{
    if (!isSQLiteSidecar(path)) {
        return false;
    }

    const std::string name = path.filename().string();
    constexpr std::array<const char*, 3> suffixes{
        "-journal",
        "-wal",
        "-shm"
    };
    for (const char* suffix : suffixes) {
        const size_t suffix_size = std::char_traits<char>::length(suffix);
        if (name.size() <= suffix_size) {
            continue;
        }

        std::string lower_name = name;
        std::transform(
            lower_name.begin(),
            lower_name.end(),
            lower_name.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            }
        );
        if (!lower_name.ends_with(suffix)) {
            continue;
        }

        const fs::path database =
            path.parent_path() /
            name.substr(0, name.size() - suffix_size);
        std::error_code error;
        return
            fs::is_regular_file(database, error) &&
            !error &&
            isSQLiteDatabaseCandidate(database);
    }
    return false;
}

CompareResult collectAutoTree(const fs::path& root,
                              std::map<fs::path, TreeEntryKind>& entries,
                              std::string& error_message)
{
    std::error_code error;
    for (fs::recursive_directory_iterator it(
             root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            error_message =
                "cannot enumerate \"" + pathToUtf8(root) + "\": " +
                error.message();
            return CompareResult::Error;
        }

        std::error_code type_error;
        if (it->is_symlink(type_error)) {
            if (type_error) {
                error_message =
                    "cannot inspect \"" + pathToUtf8(it->path()) + "\": " +
                    type_error.message();
                return CompareResult::Error;
            }
            continue;
        }

        if (isManagedSQLiteSidecar(it->path())) {
            continue;
        }

        const fs::path relative = fs::relative(it->path(), root, error);
        if (error) {
            error_message =
                "cannot make relative path for \"" + pathToUtf8(it->path()) +
                "\": " + error.message();
            return CompareResult::Error;
        }

        if (it->is_directory(type_error)) {
            entries.emplace(relative, TreeEntryKind::Directory);
        } else if (it->is_regular_file(type_error)) {
            entries.emplace(relative, TreeEntryKind::File);
        } else if (type_error) {
            error_message =
                "cannot inspect \"" + pathToUtf8(it->path()) + "\": " +
                type_error.message();
            return CompareResult::Error;
        } else {
            error_message =
                "unsupported filesystem entry \"" +
                pathToUtf8(it->path()) + '"';
            return CompareResult::Error;
        }
    }
    return CompareResult::Equal;
}

bool copyAutoFile(const fs::path& source,
                  const fs::path& destination,
                  const fs::path& relative,
                  BackupCache& cache,
                  std::vector<SnapshotFileRecord>& records,
                  std::string& error_message)
{
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        error_message =
            "cannot create destination directory: " + error.message();
        return false;
    }

    const bool sqlite = isSQLiteDatabaseCandidate(source);
    const BackupCacheResult cache_result =
        cache.materialize(source, destination, relative);
    if (!cache_result.ok) {
        error_message =
            (sqlite ? "SQLite backup failed for \"" : "file backup failed for \"") +
            pathToUtf8(relative) + "\": " + cache_result.message;
        return false;
    }

    records.push_back(
        SnapshotFileRecord{
            relative,
            cache_result.method,
            cache_result.size,
            sqlite,
            cache_result.source_fingerprint,
            cache_result.fingerprint_reliable
        }
    );
    return true;
}

bool copyAutoSource(const BackupTarget& target,
                    const fs::path& snapshot_data,
                    BackupCache& cache,
                    std::vector<SnapshotFileRecord>& records,
                    std::string& error_message)
{
    std::error_code error;
    fs::create_directories(snapshot_data, error);
    if (error) {
        error_message =
            "cannot create snapshot data directory: " + error.message();
        return false;
    }

    if (!target.is_directory) {
        return copyAutoFile(
            target.src,
            snapshot_data / target.src.filename(),
            target.src.filename(),
            cache,
            records,
            error_message
        );
    }

    std::map<fs::path, TreeEntryKind> before;
    if (collectAutoTree(target.src, before, error_message) ==
        CompareResult::Error)
    {
        return false;
    }

    for (const auto& [relative, kind] : before) {
        const fs::path source = target.src / relative;
        const fs::path destination = snapshot_data / relative;
        if (kind == TreeEntryKind::Directory) {
            fs::create_directories(destination, error);
            if (error) {
                error_message =
                    "cannot create snapshot directory \"" +
                    pathToUtf8(relative) + "\": " + error.message();
                return false;
            }
            continue;
        }

        if (!copyAutoFile(
                source,
                destination,
                relative,
                cache,
                records,
                error_message
            ))
        {
            return false;
        }
    }

    std::map<fs::path, TreeEntryKind> after;
    if (collectAutoTree(target.src, after, error_message) ==
        CompareResult::Error)
    {
        return false;
    }
    if (before != after) {
        error_message =
            "source directory entries changed while the snapshot was created";
        return false;
    }
    return true;
}

bool readManifestSourceStates(
    const fs::path& manifest,
    std::map<std::string, std::string>& states,
    std::string& error_message)
{
    std::ifstream stream(manifest, std::ios::binary);
    if (!stream.is_open()) {
        error_message =
            "cannot open manifest \"" + pathToUtf8(manifest) + '"';
        return false;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.starts_with("source_state=")) {
            continue;
        }
        constexpr size_t prefix_size =
            std::char_traits<char>::length("source_state=");
        const size_t delimiter = line.find('\t', prefix_size);
        if (delimiter == std::string::npos ||
            delimiter == prefix_size ||
            delimiter + 1 == line.size())
        {
            error_message = "invalid source_state entry in manifest";
            return false;
        }

        const std::string fingerprint =
            line.substr(prefix_size, delimiter - prefix_size);
        const std::string relative_path = line.substr(delimiter + 1);
        if (!states.emplace(relative_path, fingerprint).second) {
            error_message = "duplicate source_state entry in manifest";
            return false;
        }
    }
    if (!stream.eof()) {
        error_message = "cannot read snapshot manifest";
        return false;
    }
    return true;
}

CompareResult compareAutoSourceToSnapshot(
    const BackupTarget& target,
    const fs::path& snapshot,
    std::string& error_message)
{
    const fs::path snapshot_data = snapshot / "data";
    std::map<std::string, std::string> manifest_states;
    if (!readManifestSourceStates(
            snapshot / "manifest.txt",
            manifest_states,
            error_message
        ))
    {
        return CompareResult::Different;
    }

    std::map<fs::path, TreeEntryKind> source_tree;
    std::map<fs::path, TreeEntryKind> snapshot_tree;
    std::vector<fs::path> relative_files;
    if (target.is_directory) {
        if (collectAutoTree(target.src, source_tree, error_message) ==
                CompareResult::Error ||
            collectTree(snapshot_data, snapshot_tree, error_message) ==
                CompareResult::Error)
        {
            return CompareResult::Error;
        }
        if (source_tree != snapshot_tree) {
            return CompareResult::Different;
        }
        for (const auto& [relative, kind] : source_tree) {
            if (kind == TreeEntryKind::File) {
                relative_files.push_back(relative);
            }
        }
    } else {
        std::error_code error;
        const fs::path copied = snapshot_data / target.src.filename();
        if (!fs::is_regular_file(copied, error) || error) {
            return CompareResult::Different;
        }
        relative_files.push_back(target.src.filename());
    }

    std::map<fs::path, std::string> observed_states;
    for (const fs::path& relative : relative_files) {
        const fs::path source =
            target.is_directory ? target.src / relative : target.src;
        const fs::path copied = snapshot_data / relative;
        const BackupSourceState state = inspectBackupSource(source);
        if (!state.ok) {
            error_message =
                "cannot inspect \"" + pathToUtf8(relative) +
                "\": " + state.message;
            return CompareResult::Error;
        }
        if (!state.reliable) {
            return CompareResult::Different;
        }

        const auto manifest_it =
            manifest_states.find(pathToUtf8(relative));
        if (manifest_it == manifest_states.end() ||
            manifest_it->second != state.fingerprint)
        {
            return CompareResult::Different;
        }

        if (state.sqlite) {
            std::string integrity_error;
            if (!verifySQLiteDatabase(copied, integrity_error)) {
                return CompareResult::Different;
            }
        } else {
            std::string compare_error;
            const CompareResult comparison =
                compareFiles(source, copied, compare_error);
            if (comparison == CompareResult::Error) {
                error_message = compare_error;
                return CompareResult::Error;
            }
            if (comparison == CompareResult::Different) {
                return CompareResult::Different;
            }
        }
        observed_states.emplace(relative, state.fingerprint);
    }

    if (target.is_directory) {
        std::map<fs::path, TreeEntryKind> source_tree_after;
        if (collectAutoTree(
                target.src,
                source_tree_after,
                error_message
            ) == CompareResult::Error)
        {
            return CompareResult::Error;
        }
        if (source_tree_after != source_tree) {
            return CompareResult::Different;
        }
    }

    for (const auto& [relative, observed] : observed_states) {
        const fs::path source =
            target.is_directory ? target.src / relative : target.src;
        const BackupSourceState state = inspectBackupSource(source);
        if (!state.ok) {
            error_message =
                "cannot re-inspect \"" + pathToUtf8(relative) +
                "\": " + state.message;
            return CompareResult::Error;
        }
        if (!state.reliable || state.fingerprint != observed) {
            return CompareResult::Different;
        }
    }
    return CompareResult::Equal;
}

bool copySourceToSnapshot(const BackupTarget& target,
                          const fs::path& snapshot_data,
                          std::error_code& error)
{
    fs::create_directories(snapshot_data, error);
    if (error) {
        return false;
    }

    if (target.is_directory) {
        fs::copy(
            target.src,
            snapshot_data,
            fs::copy_options::recursive |
                fs::copy_options::overwrite_existing |
                fs::copy_options::skip_symlinks,
            error
        );
    } else {
        fs::copy_file(
            target.src,
            snapshot_data / target.src.filename(),
            fs::copy_options::overwrite_existing,
            error
        );
    }
    return !error;
}

bool collectFilesystemRecords(
    const BackupTarget& target,
    const fs::path& snapshot_data,
    std::vector<SnapshotFileRecord>& records,
    std::string& error_message)
{
    std::error_code error;
    if (!target.is_directory) {
        const fs::path copied = snapshot_data / target.src.filename();
        const auto size = fs::file_size(copied, error);
        if (error) {
            error_message =
                "cannot read copied file size: " + error.message();
            return false;
        }
        records.push_back(
            SnapshotFileRecord{
                target.src.filename(),
                "filesystem-copy",
                size,
                false,
                {},
                false
            }
        );
        return true;
    }

    for (fs::recursive_directory_iterator it(
             snapshot_data,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            error_message =
                "cannot enumerate copied files: " + error.message();
            return false;
        }
        if (!it->is_regular_file(error)) {
            if (error) {
                error_message =
                    "cannot inspect copied file: " + error.message();
                return false;
            }
            continue;
        }

        const fs::path relative =
            fs::relative(it->path(), snapshot_data, error);
        if (error) {
            error_message =
                "cannot make copied file path relative: " + error.message();
            return false;
        }
        const auto size = it->file_size(error);
        if (error) {
            error_message =
                "cannot read copied file size: " + error.message();
            return false;
        }
        records.push_back(
            SnapshotFileRecord{
                relative,
                "filesystem-copy",
                size,
                false,
                {},
                false
            }
        );
    }
    return true;
}

bool writeManifest(const fs::path& staging_dir,
                   const BackupTarget& target,
                   const std::string& time_str,
                   const std::vector<SnapshotFileRecord>& records,
                   std::string& error_message)
{
    const fs::path manifest = staging_dir / "manifest.txt";
    std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        error_message = "cannot create manifest";
        return false;
    }

    stream
        << "format=4\n"
        << "created=" << time_str << '\n'
        << "type=" << (target.is_directory ? "directory" : "file") << '\n'
        << "mode="
        << (target.mode == BackupMode::Auto ? "auto" : "filesystem")
        << '\n'
        << "source=" << pathToUtf8(target.src) << '\n'
        << "sqlite_version=" << backupSQLiteVersion() << '\n'
        << "cache="
        << (
            target.mode == BackupMode::Auto && target.cache
                ? "verified-per-file"
                : "disabled"
        )
        << '\n'
        << "skip_unchanged="
        << (target.skip_unchanged ? "true" : "false")
        << '\n'
        << "files=" << records.size() << '\n';
    const bool has_sqlite = std::any_of(
        records.begin(),
        records.end(),
        [](const SnapshotFileRecord& record) { return record.sqlite; }
    );
    if (has_sqlite) {
        stream << "sqlite_integrity_check=ok\n";
    }
    for (const auto& record : records) {
        stream
            << "entry="
            << record.method
            << '\t'
            << record.size
            << '\t'
            << pathToUtf8(record.relative_path)
            << '\n';
        if (record.fingerprint_reliable) {
            stream
                << "source_state="
                << record.source_fingerprint
                << '\t'
                << pathToUtf8(record.relative_path)
                << '\n';
        }
    }
    stream.flush();
    if (!stream) {
        error_message = "cannot write manifest";
        return false;
    }
    return true;
}

fs::path chooseVersionPath(const fs::path& snapshots_root,
                           const std::string& time_str,
                           std::error_code& error)
{
    fs::path candidate = snapshots_root / time_str;
    if (!fs::exists(candidate, error)) {
        return error ? fs::path{} : candidate;
    }
    if (error) {
        return {};
    }

    for (unsigned int suffix = 1; suffix <= 999999; ++suffix) {
        std::ostringstream name;
        name
            << time_str
            << '_'
            << std::setfill('0')
            << std::setw(6)
            << suffix;
        candidate = snapshots_root / name.str();
        if (!fs::exists(candidate, error)) {
            return error ? fs::path{} : candidate;
        }
        if (error) {
            return {};
        }
    }

    error = std::make_error_code(std::errc::file_exists);
    return {};
}

fs::path chooseStagingPath(const fs::path& snapshots_root,
                           const std::string& version_name)
{
    for (;;) {
        const auto sequence = staging_sequence.fetch_add(1);
        const fs::path candidate =
            snapshots_root /
            (
                ".partial_" + version_name + "_" +
                std::to_string(sequence)
            );
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

void removeStaging(const fs::path& staging_dir)
{
    std::error_code error;
    fs::remove_all(staging_dir, error);
    if (error) {
        logWarning("remove partial snapshot", staging_dir, error.message());
    }
}

void cleanupStaleStaging(const fs::path& snapshots_root)
{
    std::error_code error;
    if (!fs::exists(snapshots_root, error) || error) {
        return;
    }

    for (fs::directory_iterator it(
             snapshots_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         it != end;
         it.increment(error))
    {
        if (error) {
            logWarning(
                "scan partial snapshots",
                snapshots_root,
                error.message()
            );
            return;
        }

        const std::string name = it->path().filename().string();
        if (!name.starts_with(".partial_")) {
            continue;
        }

        std::error_code remove_error;
        fs::remove_all(it->path(), remove_error);
        if (remove_error) {
            logWarning(
                "remove stale partial snapshot",
                it->path(),
                remove_error.message()
            );
        }
    }
}

void rotateSnapshots(const fs::path& snapshots_root, size_t max_versions)
{
    std::error_code error;
    std::vector<fs::path> snapshots = listSnapshots(snapshots_root, error);
    if (error) {
        logWarning("list snapshots for rotation", snapshots_root, error.message());
        return;
    }

    if (snapshots.size() <= max_versions) {
        return;
    }

    const size_t remove_count = snapshots.size() - max_versions;
    for (size_t index = 0; index < remove_count; ++index) {
        fs::remove_all(snapshots[index], error);
        if (error) {
            logWarning(
                "remove old snapshot",
                snapshots[index],
                error.message()
            );
            error.clear();
        }
    }
}

BackupTargetResult failedResult(const fs::path& source,
                                const std::string& message)
{
    return BackupTargetResult{
        source,
        BackupTargetStatus::Failed,
        {},
        message
    };
}

} // namespace

bool BackupRunResult::ok() const noexcept
{
    return
        !already_running &&
        std::none_of(
            targets.begin(),
            targets.end(),
            [](const BackupTargetResult& result) {
                return result.status == BackupTargetStatus::Failed;
            }
        );
}

BackupEngine::BackupEngine(fs::path backup_root,
                           const std::vector<BackupTarget>& targets)
    : backup_root_(std::move(backup_root)),
      targets_(targets)
{
}

BackupRunResult BackupEngine::runOnce()
{
    return runOnceAt(std::chrono::system_clock::now());
}

BackupRunResult BackupEngine::runOnceAt(
    std::chrono::system_clock::time_point timestamp)
{
    std::unique_lock lock(run_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        logLine("[WARNING] Backup run skipped: previous run is still active");
        return BackupRunResult{true, {}};
    }

    BackupRunResult run_result;
    const std::string time_str = getTimeString(timestamp);
    run_result.targets.reserve(targets_.size());

    for (const auto& target : targets_) {
        try {
            run_result.targets.push_back(
                createSnapshot(target, time_str, timestamp)
            );
        } catch (const std::exception& exception) {
            logError("backup target", target.src, exception.what());
            run_result.targets.push_back(
                failedResult(target.src, exception.what())
            );
        } catch (...) {
            constexpr const char* message = "unknown exception";
            logError("backup target", target.src, message);
            run_result.targets.push_back(failedResult(target.src, message));
        }
    }
    return run_result;
}

std::string BackupEngine::getTimeString(
    std::chrono::system_clock::time_point tp)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(tp);
    std::tm local_time{};
#ifdef _WIN32
    if (localtime_s(&local_time, &time) != 0) {
        throw std::runtime_error("cannot convert snapshot time");
    }
#else
    if (localtime_r(&time, &local_time) == nullptr) {
        throw std::runtime_error("cannot convert snapshot time");
    }
#endif

    char buffer[32];
    if (std::strftime(
            buffer,
            sizeof(buffer),
            "%Y%m%d_%H%M%S",
            &local_time
        ) == 0)
    {
        throw std::runtime_error("cannot format snapshot time");
    }
    return buffer;
}

BackupTargetResult BackupEngine::createSnapshot(
    const BackupTarget& configured_target,
    const std::string& time_str,
    std::chrono::system_clock::time_point timestamp)
{
    std::error_code error;
    const fs::path normalized_source =
        normalizedAbsolute(configured_target.src, error);
    if (error) {
        logError("absolute source path", configured_target.src, error);
        return failedResult(configured_target.src, error.message());
    }

    const fs::path normalized_backup_root =
        normalizedAbsolute(backup_root_, error);
    if (error) {
        logError("absolute backup path", backup_root_, error);
        return failedResult(configured_target.src, error.message());
    }

    BackupTarget target = configured_target;
    target.src = normalized_source;

    if (target.max_versions == 0) {
        const std::string message = "max_versions must be greater than zero";
        logError("validate target", target.src, message);
        return failedResult(target.src, message);
    }

    if (!fs::exists(target.src, error)) {
        const std::string message =
            error ? error.message() : "source does not exist";
        logError("validate target", target.src, message);
        return failedResult(target.src, message);
    }
    if (error) {
        logError("validate target", target.src, error);
        return failedResult(target.src, error.message());
    }

    const bool actual_is_directory = fs::is_directory(target.src, error);
    if (error) {
        logError("inspect target", target.src, error);
        return failedResult(target.src, error.message());
    }
    const bool actual_is_file = fs::is_regular_file(target.src, error);
    if (error) {
        logError("inspect target", target.src, error);
        return failedResult(target.src, error.message());
    }

    if (target.is_directory != actual_is_directory ||
        (!target.is_directory && !actual_is_file))
    {
        const std::string message =
            target.is_directory
                ? "configured as directory, but source is not a directory"
                : "configured as file, but source is not a regular file";
        logError("validate target", target.src, message);
        return failedResult(target.src, message);
    }

    if (target.is_directory &&
        pathIsWithin(normalized_backup_root, normalized_source))
    {
        const std::string message =
            "backup directory must not be inside the source directory";
        logError("validate target", target.src, message);
        return failedResult(target.src, message);
    }
    if (!target.is_directory &&
        componentsEqual(normalized_backup_root, normalized_source))
    {
        const std::string message =
            "backup directory must not be the source file";
        logError("validate target", target.src, message);
        return failedResult(target.src, message);
    }

    const fs::path target_root =
        normalized_backup_root / targetDirectoryName(normalized_source);
    if (target.strategy == BackupStrategy::MirrorHistory) {
        if (target.history_tiers.empty()) {
            const std::string message =
                "mirror_history requires at least one history period";
            logError("validate target", target.src, message);
            return failedResult(target.src, message);
        }
        return updateMirrorHistory(
            target,
            target_root,
            time_str,
            timestamp
        );
    }

    const fs::path snapshots_root = target_root / "snapshots";
    fs::create_directories(snapshots_root, error);
    if (error) {
        logError("create snapshots directory", snapshots_root, error);
        return failedResult(target.src, error.message());
    }

    cleanupStaleStaging(snapshots_root);

    std::vector<fs::path> snapshots = listSnapshots(snapshots_root, error);
    if (error) {
        logError("list snapshots", snapshots_root, error);
        return failedResult(target.src, error.message());
    }

    if (target.mode == BackupMode::Filesystem && !snapshots.empty()) {
        const fs::path latest_data = snapshots.back() / "data";
        std::string compare_error;
        const CompareResult comparison =
            compareSourceToSnapshot(target, latest_data, compare_error);
        if (comparison == CompareResult::Equal) {
            logLine(
                "Backup unchanged: " + pathToUtf8(target.src) +
                " latest=" + pathToUtf8(snapshots.back())
            );
            return BackupTargetResult{
                target.src,
                BackupTargetStatus::Unchanged,
                snapshots.back(),
                "source is unchanged"
            };
        }
        if (comparison == CompareResult::Error) {
            logWarning(
                "compare latest snapshot",
                snapshots.back(),
                compare_error
            );
        }
    }

    if (target.mode == BackupMode::Auto &&
        target.skip_unchanged &&
        !snapshots.empty())
    {
        std::string compare_error;
        const CompareResult comparison =
            compareAutoSourceToSnapshot(
                target,
                snapshots.back(),
                compare_error
            );
        if (comparison == CompareResult::Equal) {
            logLine(
                "Backup unchanged: " + pathToUtf8(target.src) +
                " latest=" + pathToUtf8(snapshots.back())
            );
            return BackupTargetResult{
                target.src,
                BackupTargetStatus::Unchanged,
                snapshots.back(),
                "source is unchanged"
            };
        }
        if (comparison == CompareResult::Error) {
            logWarning(
                "inspect source for unchanged backup",
                target.src,
                compare_error
            );
        }
    }

    const fs::path final_snapshot =
        chooseVersionPath(snapshots_root, time_str, error);
    if (error) {
        logError("choose snapshot name", snapshots_root, error);
        return failedResult(target.src, error.message());
    }

    const fs::path staging_dir =
        chooseStagingPath(snapshots_root, final_snapshot.filename().string());
    const fs::path staging_data = staging_dir / "data";

    std::vector<SnapshotFileRecord> records;
    if (target.mode == BackupMode::Auto) {
        BackupCache cache(target_root / "cache", target.cache);
        std::string copy_error;
        if (!copyAutoSource(
                target,
                staging_data,
                cache,
                records,
                copy_error
            ))
        {
            logError("copy snapshot", target.src, copy_error);
            removeStaging(staging_dir);
            return failedResult(target.src, copy_error);
        }
    } else {
        if (!copySourceToSnapshot(target, staging_data, error)) {
            logError("copy snapshot", target.src, error);
            removeStaging(staging_dir);
            return failedResult(target.src, error.message());
        }

        std::string compare_error;
        const CompareResult verification =
            compareSourceToSnapshot(target, staging_data, compare_error);
        if (verification != CompareResult::Equal) {
            const std::string message =
                verification == CompareResult::Different
                    ? "source changed while the snapshot was being created"
                    : compare_error;
            logError("verify snapshot", target.src, message);
            removeStaging(staging_dir);
            return failedResult(target.src, message);
        }

        std::string records_error;
        if (!collectFilesystemRecords(
                target,
                staging_data,
                records,
                records_error
            ))
        {
            logError("collect snapshot manifest", target.src, records_error);
            removeStaging(staging_dir);
            return failedResult(target.src, records_error);
        }
    }

    std::string manifest_error;
    if (!writeManifest(
            staging_dir,
            target,
            time_str,
            records,
            manifest_error
        ))
    {
        logError("write snapshot manifest", staging_dir, manifest_error);
        removeStaging(staging_dir);
        return failedResult(target.src, manifest_error);
    }

    fs::rename(staging_dir, final_snapshot, error);
    if (error) {
        logError("publish snapshot", final_snapshot, error);
        removeStaging(staging_dir);
        return failedResult(target.src, error.message());
    }

    rotateSnapshots(snapshots_root, target.max_versions);
    logLine(
        "Snapshot created: " + pathToUtf8(target.src) +
        " -> " + pathToUtf8(final_snapshot)
    );

    return BackupTargetResult{
        target.src,
        BackupTargetStatus::SnapshotCreated,
        final_snapshot,
        "snapshot created"
    };
}
