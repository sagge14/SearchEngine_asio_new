#include "BackupCache.h"

#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct FileFingerprint {
    bool ok = false;
    std::string value;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
    std::string message;
};

struct CacheMetadata {
    std::string relative_path;
    std::string source_fingerprint;
    std::uint64_t artifact_size = 0;
    std::uint64_t artifact_hash = 0;
    bool sqlite = false;
};

struct CacheLookup {
    bool found = false;
    fs::path generation;
    fs::path artifact;
};

std::atomic<unsigned long long> cache_sequence{0};

std::string pathToUtf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

std::string normalizedPathKey(const fs::path& path)
{
    std::wstring value = path.lexically_normal().generic_wstring();
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        }
    );
#endif
    return encoding::wstring_to_utf8(value);
}

std::uint64_t fnv1a64(const char* data,
                      size_t size,
                      std::uint64_t initial = 14695981039346656037ull)
{
    std::uint64_t hash = initial;
    for (size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(data[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t fnv1a64(const std::string& value)
{
    return fnv1a64(value.data(), value.size());
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream stream;
    stream
        << std::hex
        << std::setfill('0')
        << std::setw(16)
        << value;
    return stream.str();
}

bool hashFile(const fs::path& path,
              std::uint64_t& size,
              std::uint64_t& hash,
              std::string& error_message)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error_message = "cannot open \"" + pathToUtf8(path) + "\"";
        return false;
    }

    hash = 14695981039346656037ull;
    size = 0;
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size())
        );
        const auto count = stream.gcount();
        if (count > 0) {
            hash = fnv1a64(
                buffer.data(),
                static_cast<size_t>(count),
                hash
            );
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (stream.bad()) {
        error_message =
            "I/O error while reading \"" + pathToUtf8(path) + "\"";
        return false;
    }
    return true;
}

FileFingerprint fingerprintStableFile(const fs::path& source)
{
    constexpr int max_attempts = 5;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        std::error_code error;
        const auto size_before = fs::file_size(source, error);
        if (error) {
            return FileFingerprint{
                false,
                {},
                0,
                0,
                "cannot read source size: " + error.message()
            };
        }
        const auto time_before = fs::last_write_time(source, error);
        if (error) {
            return FileFingerprint{
                false,
                {},
                0,
                0,
                "cannot read source timestamp: " + error.message()
            };
        }

        std::uint64_t first_size = 0;
        std::uint64_t first_hash = 0;
        std::string hash_error;
        if (!hashFile(source, first_size, first_hash, hash_error)) {
            return FileFingerprint{false, {}, 0, 0, hash_error};
        }

        std::uint64_t second_size = 0;
        std::uint64_t second_hash = 0;
        if (!hashFile(source, second_size, second_hash, hash_error)) {
            return FileFingerprint{false, {}, 0, 0, hash_error};
        }

        const auto size_after = fs::file_size(source, error);
        if (error) {
            return FileFingerprint{
                false,
                {},
                0,
                0,
                "cannot re-read source size: " + error.message()
            };
        }
        const auto time_after = fs::last_write_time(source, error);
        if (error) {
            return FileFingerprint{
                false,
                {},
                0,
                0,
                "cannot re-read source timestamp: " + error.message()
            };
        }

        if (size_before == size_after &&
            time_before == time_after &&
            first_size == second_size &&
            first_hash == second_hash)
        {
            std::ostringstream value;
            value
                << "file-v1:"
                << second_size
                << ':'
                << time_after.time_since_epoch().count()
                << ':'
                << hex64(second_hash);
            return FileFingerprint{
                true,
                value.str(),
                second_size,
                second_hash,
                {}
            };
        }

        if (attempt != max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    return FileFingerprint{
        false,
        {},
        0,
        0,
        "source changed during all fingerprint attempts"
    };
}

bool compareFiles(const fs::path& lhs,
                  const fs::path& rhs,
                  std::string& error_message)
{
    std::uint64_t lhs_size = 0;
    std::uint64_t lhs_hash = 0;
    if (!hashFile(lhs, lhs_size, lhs_hash, error_message)) {
        return false;
    }
    std::uint64_t rhs_size = 0;
    std::uint64_t rhs_hash = 0;
    if (!hashFile(rhs, rhs_size, rhs_hash, error_message)) {
        return false;
    }
    if (lhs_size != rhs_size || lhs_hash != rhs_hash) {
        error_message = "copied file differs from its source";
        return false;
    }
    return true;
}

bool copyStableFile(const fs::path& source,
                    const fs::path& destination,
                    std::string& error_message)
{
    constexpr int max_attempts = 5;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        std::error_code error;
        const auto size_before = fs::file_size(source, error);
        if (error) {
            error_message =
                "cannot read source size: " + error.message();
            return false;
        }
        const auto time_before = fs::last_write_time(source, error);
        if (error) {
            error_message =
                "cannot read source timestamp: " + error.message();
            return false;
        }

        fs::create_directories(destination.parent_path(), error);
        if (error) {
            error_message =
                "cannot create destination directory: " + error.message();
            return false;
        }
        fs::copy_file(
            source,
            destination,
            fs::copy_options::overwrite_existing,
            error
        );
        if (error) {
            error_message = "cannot copy file: " + error.message();
            return false;
        }

        const auto size_after = fs::file_size(source, error);
        if (error) {
            error_message =
                "cannot re-read source size: " + error.message();
            return false;
        }
        const auto time_after = fs::last_write_time(source, error);
        if (error) {
            error_message =
                "cannot re-read source timestamp: " + error.message();
            return false;
        }

        std::string compare_error;
        if (size_before == size_after &&
            time_before == time_after &&
            compareFiles(source, destination, compare_error))
        {
            return true;
        }
        if (!compare_error.empty() &&
            compare_error != "copied file differs from its source")
        {
            error_message = compare_error;
            return false;
        }

        fs::remove(destination, error);
        if (error) {
            error_message =
                "cannot remove unstable copy: " + error.message();
            return false;
        }
        if (attempt != max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    error_message = "source changed during all copy attempts";
    return false;
}

bool writeMetadata(const fs::path& path,
                   const CacheMetadata& metadata,
                   std::string& error_message)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        error_message = "cannot create cache metadata";
        return false;
    }
    stream
        << "format=1\n"
        << "relative_path=" << metadata.relative_path << '\n'
        << "source_fingerprint=" << metadata.source_fingerprint << '\n'
        << "artifact_size=" << metadata.artifact_size << '\n'
        << "artifact_hash=" << hex64(metadata.artifact_hash) << '\n'
        << "sqlite=" << (metadata.sqlite ? 1 : 0) << '\n';
    stream.flush();
    if (!stream) {
        error_message = "cannot write cache metadata";
        return false;
    }
    return true;
}

bool readMetadata(const fs::path& path, CacheMetadata& metadata)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(stream, line)) {
        const size_t delimiter = line.find('=');
        if (delimiter == std::string::npos) {
            return false;
        }
        values[line.substr(0, delimiter)] = line.substr(delimiter + 1);
    }
    if (!stream.eof() ||
        values["format"] != "1" ||
        values["source_fingerprint"].empty())
    {
        return false;
    }

    try {
        metadata.source_fingerprint = values.at("source_fingerprint");
        metadata.relative_path = values.at("relative_path");
        metadata.artifact_size = std::stoull(values.at("artifact_size"));
        metadata.artifact_hash =
            std::stoull(values.at("artifact_hash"), nullptr, 16);
        if (values.at("sqlite") == "1") {
            metadata.sqlite = true;
        } else if (values.at("sqlite") == "0") {
            metadata.sqlite = false;
        } else {
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool validateArtifact(const fs::path& artifact,
                      const CacheMetadata& metadata)
{
    std::error_code error;
    if (!fs::is_regular_file(artifact, error) || error) {
        return false;
    }
    const auto size = fs::file_size(artifact, error);
    if (error || size != metadata.artifact_size) {
        return false;
    }

    std::uint64_t hashed_size = 0;
    std::uint64_t hash = 0;
    std::string hash_error;
    if (!hashFile(artifact, hashed_size, hash, hash_error) ||
        hashed_size != metadata.artifact_size ||
        hash != metadata.artifact_hash)
    {
        return false;
    }

    if (metadata.sqlite) {
        std::string integrity_error;
        if (!verifySQLiteDatabase(artifact, integrity_error)) {
            return false;
        }
    }
    return true;
}

CacheLookup findCache(const fs::path& entry_root,
                      const std::string& relative_path,
                      const std::string& source_fingerprint,
                      bool sqlite)
{
    CacheLookup lookup;
    std::error_code error;
    if (!fs::exists(entry_root, error) || error) {
        return lookup;
    }

    std::vector<fs::path> generations;
    for (fs::directory_iterator it(
             entry_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        const std::string name = it->path().filename().string();
        if (name.starts_with(".partial_")) {
            std::error_code remove_error;
            fs::remove_all(it->path(), remove_error);
            continue;
        }
        if (it->is_directory(error) && name.starts_with("gen_")) {
            generations.push_back(it->path());
        }
    }
    if (error) {
        return lookup;
    }

    std::sort(generations.rbegin(), generations.rend());
    for (const fs::path& generation : generations) {
        CacheMetadata metadata;
        if (!readMetadata(generation / "metadata.txt", metadata) ||
            metadata.relative_path != relative_path ||
            metadata.source_fingerprint != source_fingerprint ||
            metadata.sqlite != sqlite)
        {
            continue;
        }
        const fs::path artifact = generation / "artifact";
        if (!validateArtifact(artifact, metadata)) {
            continue;
        }
        lookup.found = true;
        lookup.generation = generation;
        lookup.artifact = artifact;
        return lookup;
    }
    return lookup;
}

fs::path choosePartialGeneration(const fs::path& entry_root)
{
    for (;;) {
        const auto sequence = cache_sequence.fetch_add(1);
        const fs::path candidate =
            entry_root / (".partial_" + std::to_string(sequence));
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

fs::path chooseFinalGeneration(const fs::path& entry_root)
{
    const auto now =
        std::chrono::system_clock::now().time_since_epoch().count();
    for (;;) {
        const auto sequence = cache_sequence.fetch_add(1);
        const fs::path candidate =
            entry_root /
            (
                "gen_" + std::to_string(now) + "_" +
                std::to_string(sequence)
            );
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

void cleanupOtherGenerations(const fs::path& entry_root,
                             const fs::path& keep)
{
    std::error_code error;
    for (fs::directory_iterator it(
             entry_root,
             fs::directory_options::skip_permission_denied,
             error
         ),
         end;
         !error && it != end;
         it.increment(error))
    {
        if (it->path() == keep) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (!name.starts_with("gen_") && !name.starts_with(".partial_")) {
            continue;
        }
        std::error_code remove_error;
        fs::remove_all(it->path(), remove_error);
    }
}

BackupCacheResult materializeDirect(
    const fs::path& source,
    const fs::path& destination,
    bool sqlite,
    const std::string& cache_error)
{
    if (sqlite) {
        const SQLiteBackupResult result =
            backupSQLiteDatabase(source, destination);
        if (!result.ok) {
            return BackupCacheResult{
                false,
                {},
                0,
                result.message
            };
        }
        return BackupCacheResult{
            true,
            "sqlite-online",
            result.size,
            cache_error
        };
    }

    std::string copy_error;
    if (!copyStableFile(source, destination, copy_error)) {
        return BackupCacheResult{false, {}, 0, copy_error};
    }
    std::error_code error;
    const auto size = fs::file_size(destination, error);
    if (error) {
        return BackupCacheResult{
            false,
            {},
            0,
            "cannot read copied file size: " + error.message()
        };
    }
    return BackupCacheResult{
        true,
        "stable-file",
        size,
        cache_error
    };
}

} // namespace

BackupSourceState inspectBackupSource(const fs::path& source)
{
    const bool sqlite = isSQLiteDatabaseCandidate(source);
    if (sqlite) {
        const SQLiteSourceFingerprint fingerprint =
            inspectSQLiteSource(source);
        return BackupSourceState{
            fingerprint.ok,
            fingerprint.ok && fingerprint.cacheable,
            true,
            fingerprint.value,
            fingerprint.message
        };
    }

    const FileFingerprint fingerprint = fingerprintStableFile(source);
    return BackupSourceState{
        fingerprint.ok,
        fingerprint.ok,
        false,
        fingerprint.value,
        fingerprint.message
    };
}

BackupCache::BackupCache(fs::path root, bool enabled)
    : root_(std::move(root)),
      enabled_(enabled)
{
}

BackupCacheResult BackupCache::materialize(
    const fs::path& source,
    const fs::path& destination,
    const fs::path& relative_path)
{
    const bool sqlite = isSQLiteDatabaseCandidate(source);

    std::string source_fingerprint;
    bool source_cacheable = false;
    SQLiteSourceFingerprint sqlite_before;
    FileFingerprint file_before;
    if (sqlite) {
        sqlite_before = inspectSQLiteSource(source);
        if (!sqlite_before.ok) {
            return BackupCacheResult{
                false,
                {},
                0,
                sqlite_before.message
            };
        }
        source_cacheable = sqlite_before.cacheable;
        source_fingerprint = sqlite_before.value;
    } else {
        file_before = fingerprintStableFile(source);
        if (!file_before.ok) {
            return BackupCacheResult{
                false,
                {},
                0,
                file_before.message
            };
        }
        source_cacheable = true;
        source_fingerprint = file_before.value;
    }

    if (!enabled_) {
        BackupCacheResult direct =
            materializeDirect(source, destination, sqlite, "cache disabled");
        if (!direct.ok) {
            return direct;
        }

        const BackupSourceState after = inspectBackupSource(source);
        if (after.ok &&
            after.reliable &&
            source_cacheable &&
            after.fingerprint == source_fingerprint)
        {
            direct.source_fingerprint = after.fingerprint;
            direct.fingerprint_reliable = true;
        } else {
            direct.message =
                "source changed during direct backup; unchanged state "
                "was not recorded";
        }
        return direct;
    }

    const fs::path entry_root =
        root_ / "files" / hex64(fnv1a64(normalizedPathKey(relative_path)));
    const std::string relative_key = normalizedPathKey(relative_path);
    if (source_cacheable) {
        const CacheLookup lookup =
            findCache(
                entry_root,
                relative_key,
                source_fingerprint,
                sqlite
            );
        if (lookup.found) {
            std::string copy_error;
            if (copyStableFile(lookup.artifact, destination, copy_error)) {
                bool source_still_matches = false;
                if (sqlite) {
                    const SQLiteSourceFingerprint after =
                        inspectSQLiteSource(source);
                    source_still_matches =
                        after.ok &&
                        after.cacheable &&
                        after.value == source_fingerprint;
                } else {
                    const FileFingerprint after =
                        fingerprintStableFile(source);
                    source_still_matches =
                        after.ok &&
                        after.value == source_fingerprint;
                }

                if (source_still_matches) {
                    std::error_code error;
                    const auto size = fs::file_size(destination, error);
                    if (!error) {
                        return BackupCacheResult{
                            true,
                            sqlite ? "sqlite-cache" : "file-cache",
                            size,
                            "verified cache hit",
                            source_fingerprint,
                            true
                        };
                    }
                }
            }

            std::error_code remove_error;
            fs::remove(destination, remove_error);
        }
    }

    std::error_code error;
    fs::create_directories(entry_root, error);
    if (error) {
        return materializeDirect(
            source,
            destination,
            sqlite,
            "cache unavailable: " + error.message()
        );
    }

    const fs::path partial = choosePartialGeneration(entry_root);
    fs::create_directories(partial, error);
    if (error) {
        return materializeDirect(
            source,
            destination,
            sqlite,
            "cache unavailable: " + error.message()
        );
    }
    const fs::path partial_artifact = partial / "artifact";

    BackupCacheResult fresh;
    if (sqlite) {
        const SQLiteBackupResult backup =
            backupSQLiteDatabase(source, partial_artifact);
        if (!backup.ok) {
            fs::remove_all(partial, error);
            return BackupCacheResult{false, {}, 0, backup.message};
        }
        fresh = BackupCacheResult{
            true,
            "sqlite-online",
            backup.size,
            {}
        };
    } else {
        std::string copy_error;
        if (!copyStableFile(source, partial_artifact, copy_error)) {
            fs::remove_all(partial, error);
            return BackupCacheResult{false, {}, 0, copy_error};
        }
        const auto size = fs::file_size(partial_artifact, error);
        if (error) {
            fs::remove_all(partial, error);
            return BackupCacheResult{
                false,
                {},
                0,
                "cannot read cached file size: " + error.message()
            };
        }
        fresh = BackupCacheResult{true, "stable-file", size, {}};
    }

    std::uint64_t artifact_size = 0;
    std::uint64_t artifact_hash = 0;
    std::string hash_error;
    if (!hashFile(
            partial_artifact,
            artifact_size,
            artifact_hash,
            hash_error
        ))
    {
        fs::remove_all(partial, error);
        return BackupCacheResult{false, {}, 0, hash_error};
    }

    bool publish_cache = false;
    std::string final_source_fingerprint;
    if (sqlite) {
        const SQLiteSourceFingerprint sqlite_after =
            inspectSQLiteSource(source);
        publish_cache =
            sqlite_after.ok &&
            sqlite_before.cacheable &&
            sqlite_after.cacheable &&
            sqlite_before.value == sqlite_after.value;
        final_source_fingerprint = sqlite_after.value;
    } else {
        const FileFingerprint file_after =
            fingerprintStableFile(source);
        publish_cache =
            file_after.ok &&
            file_after.size == artifact_size &&
            file_after.hash == artifact_hash;
        final_source_fingerprint = file_after.value;
    }

    fs::path materialization_source = partial_artifact;
    fs::path published_generation;
    if (publish_cache) {
        fresh.source_fingerprint = final_source_fingerprint;
        fresh.fingerprint_reliable = true;
        const CacheMetadata metadata{
            relative_key,
            final_source_fingerprint,
            artifact_size,
            artifact_hash,
            sqlite
        };
        std::string metadata_error;
        if (writeMetadata(
                partial / "metadata.txt",
                metadata,
                metadata_error
            ))
        {
            published_generation = chooseFinalGeneration(entry_root);
            fs::rename(partial, published_generation, error);
            if (!error) {
                materialization_source =
                    published_generation / "artifact";
            } else {
                fresh.message =
                    "cache publish failed: " + error.message();
            }
        } else {
            fresh.message = metadata_error;
        }
    } else {
        fresh.message =
            sqlite
                ? "source changed during backup; artifact was not cached"
                : "source changed after copy; artifact was not cached";
    }

    std::string copy_error;
    if (!copyStableFile(
            materialization_source,
            destination,
            copy_error
        ))
    {
        if (published_generation.empty()) {
            fs::remove_all(partial, error);
        }
        return BackupCacheResult{false, {}, 0, copy_error};
    }

    if (!published_generation.empty()) {
        cleanupOtherGenerations(entry_root, published_generation);
    } else {
        fs::remove_all(partial, error);
    }
    return fresh;
}
