#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

enum class BackupMode {
    /// Detect SQLite databases and use the Online Backup API for them.
    Auto,
    /// Copy every entry as an ordinary filesystem object.
    Filesystem
};

enum class BackupStrategy {
    /// Complete, independently restorable version directories.
    Snapshot,
    /// One current mirror plus content-addressed historical file versions.
    MirrorHistory
};

struct BackupHistoryTier {
    std::string name;
    size_t period_sec = 0;
    size_t max_points = 0;
};

struct BackupTarget {
    fs::path src;
    size_t max_versions = 5;
    bool is_directory = false;
    BackupMode mode = BackupMode::Auto;
    /// Keep verified per-file artifacts between snapshots.
    bool cache = true;
    /// Do not publish a new version when a reliable source state is unchanged.
    bool skip_unchanged = false;
    BackupStrategy strategy = BackupStrategy::Snapshot;
    std::vector<BackupHistoryTier> history_tiers;
};

struct BackupGroup {
    std::string backup_dir;
    size_t period_sec = 3600;
    std::vector<BackupTarget> targets;
};

enum class BackupTargetStatus {
    SnapshotCreated,
    MirrorUpdated,
    Unchanged,
    Failed
};

struct BackupTargetResult {
    fs::path source;
    BackupTargetStatus status = BackupTargetStatus::Failed;
    fs::path snapshot;
    std::string message;
};

struct BackupRunResult {
    bool already_running = false;
    std::vector<BackupTargetResult> targets;

    bool ok() const noexcept;
};

/// Synchronous filesystem backup engine.
///
/// Every published version is a complete, independently restorable snapshot.
/// The engine deliberately has no scheduler or Boost.Asio dependency.
class BackupEngine {
public:
    BackupEngine(fs::path backup_root, const std::vector<BackupTarget>& targets);

    BackupRunResult runOnce();

    /// Deterministic entry point used by tests and one-shot frontends.
    BackupRunResult runOnceAt(
        std::chrono::system_clock::time_point timestamp
    );

private:
    static std::string getTimeString(std::chrono::system_clock::time_point tp);
    BackupTargetResult createSnapshot(
        const BackupTarget& target,
        const std::string& time_str,
        std::chrono::system_clock::time_point timestamp
    );

    fs::path backup_root_;
    std::vector<BackupTarget> targets_;
    std::mutex run_mutex_;
};
