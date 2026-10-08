#include "Backup/BackupEngine.h"
#include "Backup/Restore/RestoreInterfaces.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace nh = nlohmann;

class TemporaryExcludeDirectory {
public:
    TemporaryExcludeDirectory()
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            fs::temp_directory_path() /
            (
                "searchengine_backup_exclude_" +
                std::to_string(unique) + "_" +
                std::to_string(sequence.fetch_add(1))
            );
        fs::create_directories(path_);
    }

    ~TemporaryExcludeDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const
    {
        return path_;
    }

private:
    fs::path path_;
};

void writeFile(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    );
}

void createSQLiteDatabase(const fs::path& path, const std::string& marker)
{
    fs::create_directories(path.parent_path());
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        const std::string message =
            database == nullptr ? "open failed" : sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    const std::string sql =
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE t(v TEXT);"
        "INSERT INTO t VALUES('" + marker + "');";
    const int result =
        sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "" : error;
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK) {
        throw std::runtime_error(message.empty() ? "sql failed" : message);
    }
}

void prepareSourceTree(const fs::path& source, bool with_sqlite)
{
    writeFile(source / "keep.txt", "keep-v1");
    writeFile(source / "nested" / "ok.txt", "nested-ok");
    fs::create_directories(source / "empty_keep");
    writeFile(source / "__astcache" / "volatile.tmp", "cache-v1");
    writeFile(source / "build" / "a.obj", "obj-v1");
    writeFile(source / "notes.tmp", "tmp-v1");
    if (with_sqlite) {
        createSQLiteDatabase(source / "data.db3", "db-v1");
    }
}

fs::path onlyTargetRoot(const fs::path& backup_root)
{
    std::vector<fs::path> targets;
    for (const auto& entry : fs::directory_iterator(backup_root)) {
        if (entry.is_directory()) {
            targets.push_back(entry.path());
        }
    }
    EXPECT_EQ(1u, targets.size());
    return targets.front();
}

std::vector<fs::path> listSnapshots(const fs::path& target_root)
{
    std::vector<fs::path> snapshots;
    const fs::path root = target_root / "snapshots";
    std::error_code error;
    if (!fs::exists(root, error) || error) {
        return snapshots;
    }
    for (const auto& entry : fs::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && !name.starts_with(".partial_")) {
            snapshots.push_back(entry.path());
        }
    }
    std::sort(snapshots.begin(), snapshots.end());
    return snapshots;
}

size_t pointCount(const fs::path& target_root, const std::string& period)
{
    const fs::path root = target_root / "restore_points" / period;
    std::error_code error;
    if (!fs::exists(root, error) || error) {
        return 0;
    }
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_directory()) {
            ++count;
        }
    }
    return count;
}

nh::json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    nh::json result;
    input >> result;
    return result;
}

bool manifestContainsExclude(const fs::path& manifest, const std::string& pattern)
{
    std::ifstream stream(manifest, std::ios::binary);
    std::string line;
    const std::string expected = "exclude=" + pattern;
    while (std::getline(stream, line)) {
        if (line == expected) {
            return true;
        }
    }
    return false;
}

void expectFilteredTree(const fs::path& data_root, bool expect_sqlite)
{
    EXPECT_EQ("keep-v1", readFile(data_root / "keep.txt"));
    EXPECT_EQ("nested-ok", readFile(data_root / "nested" / "ok.txt"));
    EXPECT_TRUE(fs::is_empty(data_root / "empty_keep"));
    EXPECT_FALSE(fs::exists(data_root / "__astcache"));
    EXPECT_FALSE(fs::exists(data_root / "build" / "a.obj"));
    EXPECT_FALSE(fs::exists(data_root / "notes.tmp"));
    if (expect_sqlite) {
        EXPECT_TRUE(fs::exists(data_root / "data.db3"));
    }
}

BackupTarget makeSnapshotTarget(const fs::path& source, BackupMode mode)
{
    BackupTarget target;
    target.src = source;
    target.max_versions = 5;
    target.is_directory = true;
    target.mode = mode;
    target.strategy = BackupStrategy::Snapshot;
    target.skip_unchanged = true;
    target.cache = true;
    target.exclude = {"__astcache/", "*.obj", "*.tmp"};
    return target;
}

BackupTarget makeMirrorTarget(const fs::path& source, BackupMode mode)
{
    BackupTarget target = makeSnapshotTarget(source, mode);
    target.strategy = BackupStrategy::MirrorHistory;
    target.history_tiers = {
        BackupHistoryTier{"every_10s", 10, 5},
        BackupHistoryTier{"every_1h", 3600, 2}
    };
    return target;
}

const auto start_time =
    std::chrono::system_clock::from_time_t(1704067200);

void runSnapshotExcludeScenario(BackupMode mode)
{
    TemporaryExcludeDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup = temporary.path() / "backup";
    prepareSourceTree(source, mode == BackupMode::Auto);

    BackupEngine engine(backup, {makeSnapshotTarget(source, mode)});
    const BackupRunResult first = engine.runOnceAt(start_time);
    ASSERT_TRUE(first.ok()) << first.targets.front().message;
    ASSERT_EQ(BackupTargetStatus::SnapshotCreated, first.targets.front().status);

    const fs::path target_root = onlyTargetRoot(backup);
    auto snapshots = listSnapshots(target_root);
    ASSERT_EQ(1u, snapshots.size());
    expectFilteredTree(snapshots.front() / "data", mode == BackupMode::Auto);
    EXPECT_TRUE(
        manifestContainsExclude(snapshots.front() / "manifest.txt", "__astcache/")
    );
    EXPECT_TRUE(
        manifestContainsExclude(snapshots.front() / "manifest.txt", "*.obj")
    );

    writeFile(source / "__astcache" / "volatile.tmp", "cache-v2");
    writeFile(source / "build" / "a.obj", "obj-v2");
    const BackupRunResult excluded_only =
        engine.runOnceAt(start_time + std::chrono::seconds(10));
    ASSERT_TRUE(excluded_only.ok()) << excluded_only.targets.front().message;
    EXPECT_EQ(
        BackupTargetStatus::Unchanged,
        excluded_only.targets.front().status
    );
    EXPECT_EQ(1u, listSnapshots(target_root).size());

    writeFile(source / "keep.txt", "keep-v2");
    const BackupRunResult included_change =
        engine.runOnceAt(start_time + std::chrono::seconds(20));
    ASSERT_TRUE(included_change.ok()) << included_change.targets.front().message;
    EXPECT_EQ(
        BackupTargetStatus::SnapshotCreated,
        included_change.targets.front().status
    );
    snapshots = listSnapshots(target_root);
    ASSERT_EQ(2u, snapshots.size());
    EXPECT_EQ("keep-v2", readFile(snapshots.back() / "data" / "keep.txt"));
    EXPECT_FALSE(fs::exists(snapshots.back() / "data" / "__astcache"));
}

void runMirrorExcludeScenario(BackupMode mode)
{
    TemporaryExcludeDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup = temporary.path() / "backup";
    prepareSourceTree(source, mode == BackupMode::Auto);

    // First create a mirror without excludes so a later exclude removes paths.
    BackupTarget initial = makeMirrorTarget(source, mode);
    initial.exclude.clear();
    {
        BackupEngine engine(backup, {initial});
        ASSERT_TRUE(engine.runOnceAt(start_time).ok());
    }

    const fs::path target_root = onlyTargetRoot(backup);
    const fs::path current_data = target_root / "current" / "data";
    EXPECT_TRUE(fs::exists(current_data / "__astcache" / "volatile.tmp"));
    EXPECT_EQ(1u, pointCount(target_root, "every_10s"));

    BackupTarget filtered = makeMirrorTarget(source, mode);
    BackupEngine engine(backup, {filtered});
    const BackupRunResult after_exclude =
        engine.runOnceAt(start_time + std::chrono::seconds(10));
    ASSERT_TRUE(after_exclude.ok()) << after_exclude.targets.front().message;
    EXPECT_EQ(
        BackupTargetStatus::MirrorUpdated,
        after_exclude.targets.front().status
    );
    expectFilteredTree(current_data, mode == BackupMode::Auto);
    const nh::json current_manifest =
        readJson(target_root / "current" / "manifest.json");
    ASSERT_TRUE(current_manifest.contains("exclude"));
    EXPECT_EQ(
        nh::json::array({"__astcache/", "*.obj", "*.tmp"}),
        current_manifest.at("exclude")
    );
    EXPECT_TRUE(current_manifest.at("complete").get<bool>());
    EXPECT_EQ(2u, pointCount(target_root, "every_10s"));

    writeFile(source / "__astcache" / "volatile.tmp", "cache-changed");
    writeFile(source / "notes.tmp", "tmp-changed");
    // Stay inside the 10s tier window so Unchanged is not masked by a new point.
    const BackupRunResult excluded_change =
        engine.runOnceAt(start_time + std::chrono::seconds(15));
    ASSERT_TRUE(excluded_change.ok()) << excluded_change.targets.front().message;
    EXPECT_EQ(
        BackupTargetStatus::Unchanged,
        excluded_change.targets.front().status
    );
    EXPECT_EQ(2u, pointCount(target_root, "every_10s"));

    writeFile(source / "keep.txt", "keep-v2");
    const BackupRunResult included_change =
        engine.runOnceAt(start_time + std::chrono::seconds(20));
    ASSERT_TRUE(included_change.ok()) << included_change.targets.front().message;
    EXPECT_EQ(
        BackupTargetStatus::MirrorUpdated,
        included_change.targets.front().status
    );
    EXPECT_EQ("keep-v2", readFile(current_data / "keep.txt"));
    EXPECT_EQ(3u, pointCount(target_root, "every_10s"));

    RestoreServices services = createMirrorHistoryRestoreServices();
    std::string error;
    auto targets = services.scanner->scanRoot(backup, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(1u, targets.size());
    auto points = services.points->listPoints(targets.front(), error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_GE(points.size(), 2u);

    RestorePointInfo oldest_historical;
    bool found_historical = false;
    for (const auto& point : points) {
        if (point.is_current) {
            continue;
        }
        if (!found_historical ||
            point.unix_seconds < oldest_historical.unix_seconds)
        {
            oldest_historical = point;
            found_historical = true;
        }
    }
    ASSERT_TRUE(found_historical);
    RestorePointInfo latest =
        services.points->currentPoint(targets.front(), error);
    ASSERT_TRUE(error.empty()) << error;

    const fs::path restored_old = temporary.path() / "restored_old";
    RestoreRequest old_request;
    old_request.point = oldest_historical;
    old_request.destination = restored_old;
    ASSERT_TRUE(services.executor->restore(old_request, nullptr, error))
        << error;
    EXPECT_TRUE(fs::exists(restored_old / "__astcache" / "volatile.tmp"));

    const fs::path restored_new = temporary.path() / "restored_new";
    RestoreRequest new_request;
    new_request.point = latest;
    new_request.destination = restored_new;
    ASSERT_TRUE(services.executor->restore(new_request, nullptr, error))
        << error;
    EXPECT_FALSE(fs::exists(restored_new / "__astcache"));
    EXPECT_EQ("keep-v2", readFile(restored_new / "keep.txt"));
}

TEST(BackupExclude, SnapshotFilesystem)
{
    runSnapshotExcludeScenario(BackupMode::Filesystem);
}

TEST(BackupExclude, SnapshotAuto)
{
    runSnapshotExcludeScenario(BackupMode::Auto);
}

TEST(BackupExclude, MirrorHistoryFilesystem)
{
    runMirrorExcludeScenario(BackupMode::Filesystem);
}

TEST(BackupExclude, MirrorHistoryAuto)
{
    runMirrorExcludeScenario(BackupMode::Auto);
}

TEST(BackupExclude, ExcludedChangingTreeDoesNotMarkIncomplete)
{
    TemporaryExcludeDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup = temporary.path() / "backup";
    writeFile(source / "keep.txt", "ok");
    writeFile(source / "__astcache" / "a.tmp", "1");
    writeFile(source / "__astcache" / "nested" / "b.tmp", "2");

    BackupEngine engine(backup, {makeMirrorTarget(source, BackupMode::Filesystem)});
    ASSERT_TRUE(engine.runOnceAt(start_time).ok());
    writeFile(source / "__astcache" / "a.tmp", "changed");
    writeFile(source / "__astcache" / "nested" / "b.tmp", "changed");
    writeFile(source / "__astcache" / "new.tmp", "new");

    const BackupRunResult second =
        engine.runOnceAt(start_time + std::chrono::seconds(5));
    ASSERT_TRUE(second.ok()) << second.targets.front().message;
    EXPECT_EQ(BackupTargetStatus::Unchanged, second.targets.front().status);

    const nh::json manifest =
        readJson(onlyTargetRoot(backup) / "current" / "manifest.json");
    EXPECT_TRUE(manifest.at("complete").get<bool>());
    EXPECT_TRUE(manifest.at("errors").empty());
}

} // namespace
