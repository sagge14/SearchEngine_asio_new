#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct BackupCacheResult {
    bool ok = false;
    std::string method;
    std::uint64_t size = 0;
    std::string message;
    std::string source_fingerprint;
    bool fingerprint_reliable = false;
};

struct BackupSourceState {
    bool ok = false;
    bool reliable = false;
    bool sqlite = false;
    std::string fingerprint;
    std::string message;
};

BackupSourceState inspectBackupSource(
    const std::filesystem::path& source
);

/// Persistent verified per-file cache used only while assembling snapshots.
///
/// Files are always physically copied from the cache into the snapshot.
/// Published snapshots therefore remain independently restorable.
class BackupCache {
public:
    explicit BackupCache(std::filesystem::path root, bool enabled = true);

    BackupCacheResult materialize(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::filesystem::path& relative_path
    );

private:
    std::filesystem::path root_;
    bool enabled_ = true;
};
