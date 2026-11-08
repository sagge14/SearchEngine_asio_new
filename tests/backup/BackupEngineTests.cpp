#include "Backup/BackupEngine.h"
#include "sqlite3.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            fs::temp_directory_path() /
            (
                "searchengine_backup_test_" + std::to_string(unique) + "_" +
                std::to_string(sequence.fetch_add(1))
            );
        fs::create_directories(path_);
    }

    ~TemporaryDirectory()
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

void executeSql(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message =
            error == nullptr ? sqlite3_errstr(result) : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void createSQLiteDatabase(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    sqlite3* database = nullptr;
    const int open_result =
        sqlite3_open(path.string().c_str(), &database);
    if (open_result != SQLITE_OK) {
        const std::string message =
            database == nullptr
                ? sqlite3_errstr(open_result)
                : sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }

    try {
        executeSql(
            database,
            "PRAGMA journal_mode=DELETE;"
            "CREATE TABLE events("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "payload TEXT NOT NULL);"
            "INSERT INTO events(payload) VALUES('initial');"
        );
    } catch (...) {
        sqlite3_close(database);
        throw;
    }
    sqlite3_close(database);
}

void insertSQLiteEvent(const fs::path& path, const std::string& payload)
{
    sqlite3* database = nullptr;
    const int open_result =
        sqlite3_open(path.string().c_str(), &database);
    if (open_result != SQLITE_OK) {
        const std::string message =
            database == nullptr
                ? sqlite3_errstr(open_result)
                : sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }

    try {
        std::string sql =
            "INSERT INTO events(payload) VALUES('" + payload + "');";
        executeSql(database, sql.c_str());
    } catch (...) {
        sqlite3_close(database);
        throw;
    }
    sqlite3_close(database);
}

std::string sqliteScalar(const fs::path& path, const char* sql)
{
    sqlite3* database = nullptr;
    const int open_result = sqlite3_open_v2(
        path.string().c_str(),
        &database,
        SQLITE_OPEN_READONLY,
        nullptr
    );
    if (open_result != SQLITE_OK) {
        const std::string message =
            database == nullptr
                ? sqlite3_errstr(open_result)
                : sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }

    sqlite3_stmt* statement = nullptr;
    const int prepare_result =
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (prepare_result != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }

    const int step_result = sqlite3_step(statement);
    if (step_result != SQLITE_ROW) {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const unsigned char* value = sqlite3_column_text(statement, 0);
    const std::string result =
        value == nullptr
            ? std::string()
            : reinterpret_cast<const char*>(value);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::string manifestMethodFor(const fs::path& snapshot,
                              const std::string& relative_path)
{
    std::string normalized_expected = relative_path;
    std::replace(
        normalized_expected.begin(),
        normalized_expected.end(),
        '\\',
        '/'
    );
    std::istringstream manifest(readFile(snapshot / "manifest.txt"));
    std::string line;
    while (std::getline(manifest, line)) {
        if (!line.starts_with("entry=")) {
            continue;
        }
        const size_t first_tab = line.find('\t');
        const size_t second_tab =
            first_tab == std::string::npos
                ? std::string::npos
                : line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos) {
            continue;
        }
        std::string entry_path = line.substr(second_tab + 1);
        std::replace(
            entry_path.begin(),
            entry_path.end(),
            '\\',
            '/'
        );
        if (entry_path == normalized_expected) {
            return line.substr(6, first_tab - 6);
        }
    }
    return {};
}

std::vector<fs::path> listCacheArtifacts(const fs::path& target_root)
{
    std::vector<fs::path> artifacts;
    const fs::path cache_files = target_root / "cache" / "files";
    std::error_code error;
    if (!fs::exists(cache_files, error) || error) {
        return artifacts;
    }
    for (fs::recursive_directory_iterator it(cache_files, error), end;
         !error && it != end;
         it.increment(error))
    {
        if (it->is_regular_file(error) &&
            it->path().filename() == "artifact")
        {
            artifacts.push_back(it->path());
        }
    }
    if (error) {
        throw std::runtime_error(error.message());
    }
    std::sort(artifacts.begin(), artifacts.end());
    return artifacts;
}

std::vector<fs::path> listSnapshotDirectories(const fs::path& snapshot)
{
    const fs::path snapshots_root = snapshot.parent_path();
    std::vector<fs::path> snapshots;
    std::error_code error;
    for (fs::directory_iterator it(snapshots_root, error), end;
         !error && it != end;
         it.increment(error))
    {
        const std::string name = it->path().filename().string();
        if (it->is_directory(error) && !name.starts_with(".partial_")) {
            snapshots.push_back(it->path());
        }
    }
    if (error) {
        throw std::runtime_error(error.message());
    }
    std::sort(snapshots.begin(), snapshots.end());
    return snapshots;
}

std::vector<fs::path> listTargetDirectories(const fs::path& backup_root)
{
    std::vector<fs::path> targets;
    std::error_code error;
    for (fs::directory_iterator it(backup_root, error), end;
         !error && it != end;
         it.increment(error))
    {
        if (it->is_directory(error)) {
            targets.push_back(it->path());
        }
    }
    if (error) {
        throw std::runtime_error(error.message());
    }
    std::sort(targets.begin(), targets.end());
    return targets;
}

const auto test_start_time =
    std::chrono::system_clock::from_time_t(1704067200);

TEST(BackupEngine, InitialFileRunCreatesIndependentSnapshot)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "item.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "initial");

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                3,
                false,
                BackupMode::Filesystem
            }
        }
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(1u, result.targets.size());
    ASSERT_TRUE(
        result.targets.front().status ==
        BackupTargetStatus::SnapshotCreated
    );
    const fs::path snapshot = result.targets.front().snapshot;
    EXPECT_EQ("initial", readFile(snapshot / "data" / "item.txt"));
    EXPECT_TRUE(fs::exists(snapshot / "manifest.txt"));
    EXPECT_EQ(1u, listSnapshotDirectories(snapshot).size());
}

TEST(BackupEngine, UnchangedSourceDoesNotCreateDuplicateSnapshot)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "item.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "same contents");

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                3,
                false,
                BackupMode::Filesystem
            }
        }
    );
    const BackupRunResult first = engine.runOnceAt(test_start_time);
    const BackupRunResult second =
        engine.runOnceAt(test_start_time + std::chrono::hours(1));

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(1u, second.targets.size());
    EXPECT_TRUE(
        second.targets.front().status == BackupTargetStatus::Unchanged
    );
    EXPECT_EQ(
        1u,
        listSnapshotDirectories(first.targets.front().snapshot).size()
    );
}

TEST(BackupEngine, EveryDirectoryVersionIsACompleteSnapshot)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "documents";
    const fs::path changed = source / "changed.txt";
    const fs::path unchanged = source / "unchanged.txt";
    const fs::path removed = source / "removed.txt";
    const fs::path added = source / "nested" / "added.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(changed, "changed v1");
    writeFile(unchanged, "unchanged");
    writeFile(removed, "will be removed");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, true}}
    );
    const BackupRunResult first = engine.runOnceAt(test_start_time);

    writeFile(changed, "changed v2");
    fs::remove(removed);
    writeFile(added, "new file");
    const BackupRunResult second =
        engine.runOnceAt(test_start_time + std::chrono::hours(1));

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    const fs::path first_data = first.targets.front().snapshot / "data";
    const fs::path second_data = second.targets.front().snapshot / "data";

    EXPECT_EQ("changed v1", readFile(first_data / "changed.txt"));
    EXPECT_EQ("unchanged", readFile(first_data / "unchanged.txt"));
    EXPECT_EQ("will be removed", readFile(first_data / "removed.txt"));
    EXPECT_FALSE(fs::exists(first_data / "nested" / "added.txt"));

    EXPECT_EQ("changed v2", readFile(second_data / "changed.txt"));
    EXPECT_EQ("unchanged", readFile(second_data / "unchanged.txt"));
    EXPECT_FALSE(fs::exists(second_data / "removed.txt"));
    EXPECT_EQ("new file", readFile(second_data / "nested" / "added.txt"));
}

TEST(BackupEngine, DetectsContentChangeWithSameSizeAndTimestamp)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "item.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "AAAA");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, false}}
    );
    const BackupRunResult first = engine.runOnceAt(test_start_time);
    const auto original_time = fs::last_write_time(source);

    writeFile(source, "BBBB");
    fs::last_write_time(source, original_time);
    const BackupRunResult second =
        engine.runOnceAt(test_start_time + std::chrono::hours(1));

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(
        second.targets.front().status ==
        BackupTargetStatus::SnapshotCreated
    );
    EXPECT_EQ(
        "AAAA",
        readFile(first.targets.front().snapshot / "data" / "item.txt")
    );
    EXPECT_EQ(
        "BBBB",
        readFile(second.targets.front().snapshot / "data" / "item.txt")
    );
}

TEST(BackupEngine, RotationKeepsConfiguredNumberOfCompleteSnapshots)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "item.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "version 0");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 2, false}}
    );
    BackupRunResult result = engine.runOnceAt(test_start_time);
    ASSERT_TRUE(result.ok());

    for (int version = 1; version <= 3; ++version) {
        writeFile(source, "version " + std::to_string(version));
        result = engine.runOnceAt(
            test_start_time + std::chrono::hours(version)
        );
        ASSERT_TRUE(result.ok());
    }

    const auto snapshots =
        listSnapshotDirectories(result.targets.front().snapshot);
    ASSERT_EQ(2u, snapshots.size());
    EXPECT_EQ("version 2", readFile(snapshots[0] / "data" / "item.txt"));
    EXPECT_EQ("version 3", readFile(snapshots[1] / "data" / "item.txt"));
}

TEST(BackupEngine, SameLeafNamesUseDifferentTargetDirectories)
{
    TemporaryDirectory temporary;
    const fs::path first_source =
        temporary.path() / "first" / "same.txt";
    const fs::path second_source =
        temporary.path() / "second" / "same.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(first_source, "first");
    writeFile(second_source, "second");

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{first_source, 2, false},
            BackupTarget{second_source, 2, false}
        }
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(2u, result.targets.size());
    EXPECT_NE(
        result.targets[0].snapshot.parent_path().parent_path(),
        result.targets[1].snapshot.parent_path().parent_path()
    );
    EXPECT_EQ(2u, listTargetDirectories(backup_root).size());
}

TEST(BackupEngine, RejectsBackupDirectoryInsideSource)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup_root = source / "backup";
    writeFile(source / "item.txt", "contents");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 2, true}}
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(1u, result.targets.size());
    EXPECT_TRUE(
        result.targets.front().status == BackupTargetStatus::Failed
    );
    EXPECT_FALSE(fs::exists(backup_root));
}

TEST(BackupEngine, RejectsZeroRetention)
{
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source" / "item.txt";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "contents");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 0, false}}
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(1u, result.targets.size());
    EXPECT_TRUE(
        result.targets.front().status == BackupTargetStatus::Failed
    );
    EXPECT_FALSE(fs::exists(backup_root));
}

TEST(BackupEngine, AutoModeCreatesVerifiedSQLiteSnapshotDuringWrites)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "BASES_PRD";
    const fs::path database = source / "ARCHIVE.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(database);
    writeFile(source / "EXPORT.ini", "settings");
    writeFile(source / "ARCHIVE.db3-shm", "stale sidecar");

    sqlite3* writer = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(database.string().c_str(), &writer));
    sqlite3_busy_timeout(writer, 1000);

    std::atomic<bool> stop{false};
    std::atomic<int> committed{0};
    std::thread writer_thread([&]() {
        while (!stop.load()) {
            char* error = nullptr;
            const int result = sqlite3_exec(
                writer,
                "BEGIN IMMEDIATE;"
                "INSERT INTO events(payload) VALUES('concurrent');"
                "COMMIT;",
                nullptr,
                nullptr,
                &error
            );
            if (result == SQLITE_OK) {
                committed.fetch_add(1);
            } else {
                sqlite3_exec(writer, "ROLLBACK", nullptr, nullptr, nullptr);
            }
            sqlite3_free(error);
            std::this_thread::yield();
        }
    });

    while (committed.load() < 5) {
        std::this_thread::yield();
    }

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, true, BackupMode::Auto}}
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    stop.store(true);
    writer_thread.join();
    sqlite3_close(writer);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(1u, result.targets.size());
    const fs::path snapshot = result.targets.front().snapshot;
    const fs::path snapshot_database =
        snapshot / "data" / "ARCHIVE.db3";

    EXPECT_EQ("ok", sqliteScalar(snapshot_database, "PRAGMA integrity_check"));
    EXPECT_GE(
        std::stoi(sqliteScalar(snapshot_database, "SELECT count(*) FROM events")),
        1
    );
    EXPECT_EQ(
        "settings",
        readFile(snapshot / "data" / "EXPORT.ini")
    );
    EXPECT_FALSE(fs::exists(snapshot / "data" / "ARCHIVE.db3-shm"));

    const std::string manifest = readFile(snapshot / "manifest.txt");
    EXPECT_NE(std::string::npos, manifest.find("format=4"));
    EXPECT_NE(std::string::npos, manifest.find("mode=auto"));
    EXPECT_NE(std::string::npos, manifest.find("sqlite-online"));
    EXPECT_NE(std::string::npos, manifest.find("integrity_check"));
}

TEST(BackupEngine, AutoModeTreatsWindowsThumbsDatabaseAsOrdinaryFile)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "PROGRAM";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(source / "Settings.db3");
    writeFile(source / "Thumbs.db", "OLE thumbnail cache");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, true, BackupMode::Auto}}
    );
    const BackupRunResult result =
        engine.runOnceAt(test_start_time);

    ASSERT_TRUE(result.ok());
    const fs::path snapshot = result.targets.front().snapshot;
    EXPECT_EQ(
        "OLE thumbnail cache",
        readFile(snapshot / "data" / "Thumbs.db")
    );
    EXPECT_EQ(
        "stable-file",
        manifestMethodFor(snapshot, "Thumbs.db")
    );
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(snapshot, "Settings.db3")
    );
}

TEST(BackupEngine, AutoModeReusesPersistentCacheAndSnapshotNeedsNoCache)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "BASES_PRD";
    const fs::path archive = source / "ARCHIVE.db3";
    const fs::path monthly = source / "METH_BASES" / "02-2026.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(archive);
    createSQLiteDatabase(monthly);
    writeFile(source / "EXPORT.ini", "settings");

    const std::vector<BackupTarget> targets{
        BackupTarget{source, 3, true, BackupMode::Auto}
    };
    BackupEngine first_engine(backup_root, targets);
    const BackupRunResult first =
        first_engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(first.targets.front().snapshot, "ARCHIVE.db3")
    );

    // A new engine object simulates a service restart. The cache is on disk.
    BackupEngine restarted_engine(backup_root, targets);
    const BackupRunResult second = restarted_engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    const fs::path second_snapshot = second.targets.front().snapshot;
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(second_snapshot, "ARCHIVE.db3")
    );
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(
            second_snapshot,
            "METH_BASES/02-2026.db3"
        )
    );
    EXPECT_EQ(
        "file-cache",
        manifestMethodFor(second_snapshot, "EXPORT.ini")
    );

    const fs::path target_root =
        second_snapshot.parent_path().parent_path();
    std::error_code error;
    fs::remove_all(target_root / "cache", error);
    ASSERT_FALSE(error);

    EXPECT_EQ(
        "ok",
        sqliteScalar(
            second_snapshot / "data" / "ARCHIVE.db3",
            "PRAGMA integrity_check"
        )
    );
    EXPECT_EQ(
        "ok",
        sqliteScalar(
            second_snapshot / "data" / "METH_BASES" / "02-2026.db3",
            "PRAGMA integrity_check"
        )
    );
    EXPECT_EQ(
        "settings",
        readFile(second_snapshot / "data" / "EXPORT.ini")
    );
}

TEST(BackupEngine, AutoFileSkipsUnchangedSourceWithoutCache)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "ARCHIVE.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(source);

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                5,
                false,
                BackupMode::Auto,
                false,
                true
            }
        }
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(1u, first.targets.size());
    EXPECT_TRUE(
        first.targets.front().status ==
        BackupTargetStatus::SnapshotCreated
    );

    const fs::path first_snapshot = first.targets.front().snapshot;
    const fs::path target_root =
        first_snapshot.parent_path().parent_path();
    EXPECT_FALSE(fs::exists(target_root / "cache"));
    const std::string first_manifest =
        readFile(first_snapshot / "manifest.txt");
    EXPECT_NE(
        std::string::npos,
        first_manifest.find("cache=disabled")
    );
    EXPECT_NE(
        std::string::npos,
        first_manifest.find("skip_unchanged=true")
    );
    EXPECT_NE(
        std::string::npos,
        first_manifest.find("source_state=sqlite-v1:")
    );

    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(
        second.targets.front().status == BackupTargetStatus::Unchanged
    );
    EXPECT_EQ(first_snapshot, second.targets.front().snapshot);
    EXPECT_EQ(1u, listSnapshotDirectories(first_snapshot).size());

    insertSQLiteEvent(source, "changed");
    const BackupRunResult third = engine.runOnceAt(
        test_start_time + std::chrono::minutes(2)
    );
    ASSERT_TRUE(third.ok());
    EXPECT_TRUE(
        third.targets.front().status ==
        BackupTargetStatus::SnapshotCreated
    );
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(third.targets.front().snapshot, "ARCHIVE.db3")
    );
    EXPECT_EQ(2u, listSnapshotDirectories(first_snapshot).size());
    EXPECT_FALSE(fs::exists(target_root / "cache"));
}

TEST(BackupEngine, AutoDirectorySkipsUnchangedAndRefreshesChangedArchive)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "BASES_PRD";
    const fs::path archive = source / "ARCHIVE.db3";
    const fs::path monthly = source / "METH_BASES" / "02-2026.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(archive);
    createSQLiteDatabase(monthly);
    writeFile(source / "EXPORT.ini", "settings");

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                5,
                true,
                BackupMode::Auto,
                true,
                true
            }
        }
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());

    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(
        second.targets.front().status == BackupTargetStatus::Unchanged
    );
    EXPECT_EQ(1u, listSnapshotDirectories(first.targets.front().snapshot).size());

    insertSQLiteEvent(archive, "changed-active");
    const BackupRunResult third = engine.runOnceAt(
        test_start_time + std::chrono::minutes(2)
    );
    ASSERT_TRUE(third.ok());
    const fs::path snapshot = third.targets.front().snapshot;
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(snapshot, "ARCHIVE.db3")
    );
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(snapshot, "METH_BASES/02-2026.db3")
    );
    EXPECT_EQ(2u, listSnapshotDirectories(snapshot).size());
}

TEST(BackupEngine, AutoModeRefreshesOnlyChangedSQLiteFile)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "BASES_PRD";
    const fs::path archive = source / "ARCHIVE.db3";
    const fs::path monthly = source / "METH_BASES" / "02-2026.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(archive);
    createSQLiteDatabase(monthly);

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, true, BackupMode::Auto}}
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());

    insertSQLiteEvent(archive, "changed-active-database");
    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    const fs::path snapshot = second.targets.front().snapshot;
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(snapshot, "ARCHIVE.db3")
    );
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(snapshot, "METH_BASES/02-2026.db3")
    );
    EXPECT_EQ(
        "2",
        sqliteScalar(
            snapshot / "data" / "ARCHIVE.db3",
            "SELECT count(*) FROM events"
        )
    );
}

TEST(BackupEngine, SuccessfulCacheEntriesSurviveAnotherFileFailure)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "BASES_PRD";
    const fs::path archive = source / "A_ARCHIVE.db3";
    const fs::path broken = source / "B_BROKEN.db3";
    const fs::path monthly = source / "METH_BASES" / "02-2026.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(archive);
    createSQLiteDatabase(monthly);

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 5, true, BackupMode::Auto}}
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());
    const fs::path first_snapshot = first.targets.front().snapshot;

    insertSQLiteEvent(archive, "prepared-before-failure");
    writeFile(broken, "not a database");
    const BackupRunResult failed = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(1u, listSnapshotDirectories(first_snapshot).size());

    std::error_code error;
    fs::remove(broken, error);
    ASSERT_FALSE(error);
    createSQLiteDatabase(broken);

    const BackupRunResult recovered = engine.runOnceAt(
        test_start_time + std::chrono::minutes(2)
    );
    ASSERT_TRUE(recovered.ok());
    const fs::path snapshot = recovered.targets.front().snapshot;
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(snapshot, "A_ARCHIVE.db3")
    );
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(snapshot, "B_BROKEN.db3")
    );
    EXPECT_EQ(
        "sqlite-cache",
        manifestMethodFor(snapshot, "METH_BASES/02-2026.db3")
    );
}

TEST(BackupEngine, CorruptCacheIsRejectedAndRebuilt)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "ARCHIVE.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(source);

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, false, BackupMode::Auto}}
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());
    const fs::path target_root =
        first.targets.front().snapshot.parent_path().parent_path();
    const auto artifacts = listCacheArtifacts(target_root);
    ASSERT_EQ(1u, artifacts.size());
    writeFile(artifacts.front(), "corrupt cache");

    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    const fs::path snapshot = second.targets.front().snapshot;
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(snapshot, "ARCHIVE.db3")
    );
    EXPECT_EQ(
        "ok",
        sqliteScalar(
            snapshot / "data" / "ARCHIVE.db3",
            "PRAGMA integrity_check"
        )
    );
    EXPECT_EQ(1u, listCacheArtifacts(target_root).size());
}

TEST(BackupEngine, WalDatabaseAlwaysUsesOnlineBackup)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "wal.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(source);

    sqlite3* database = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(source.string().c_str(), &database));
    executeSql(database, "PRAGMA journal_mode=WAL");
    sqlite3_close(database);

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                3,
                false,
                BackupMode::Auto,
                false,
                true
            }
        }
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(first.targets.front().snapshot, "wal.db3")
    );
    EXPECT_EQ(
        "sqlite-online",
        manifestMethodFor(second.targets.front().snapshot, "wal.db3")
    );
    EXPECT_EQ(2u, listSnapshotDirectories(first.targets.front().snapshot).size());
}

TEST(BackupEngine, CorruptLatestSnapshotIsNotTreatedAsUnchanged)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "ARCHIVE.db3";
    const fs::path backup_root = temporary.path() / "backup";
    createSQLiteDatabase(source);

    BackupEngine engine(
        backup_root,
        {
            BackupTarget{
                source,
                3,
                false,
                BackupMode::Auto,
                false,
                true
            }
        }
    );
    const BackupRunResult first =
        engine.runOnceAt(test_start_time);
    ASSERT_TRUE(first.ok());
    writeFile(
        first.targets.front().snapshot / "data" / "ARCHIVE.db3",
        "corrupt snapshot"
    );

    const BackupRunResult second = engine.runOnceAt(
        test_start_time + std::chrono::minutes(1)
    );
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(
        second.targets.front().status ==
        BackupTargetStatus::SnapshotCreated
    );
    EXPECT_EQ(
        "ok",
        sqliteScalar(
            second.targets.front().snapshot / "data" / "ARCHIVE.db3",
            "PRAGMA integrity_check"
        )
    );
}

TEST(BackupEngine, CorruptSQLiteCandidateDoesNotPublishSnapshot)
{
    TemporaryDirectory temporary;
    const fs::path source =
        temporary.path() / "source" / "broken.db3";
    const fs::path backup_root = temporary.path() / "backup";
    writeFile(source, "this is not a SQLite database");

    BackupEngine engine(
        backup_root,
        {BackupTarget{source, 3, false, BackupMode::Auto}}
    );
    const BackupRunResult result = engine.runOnceAt(test_start_time);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(1u, result.targets.size());
    EXPECT_TRUE(
        result.targets.front().status == BackupTargetStatus::Failed
    );
    EXPECT_TRUE(result.targets.front().snapshot.empty());

    const auto targets = listTargetDirectories(backup_root);
    ASSERT_EQ(1u, targets.size());
    const fs::path snapshots_root = targets.front() / "snapshots";
    size_t published = 0;
    for (const auto& entry : fs::directory_iterator(snapshots_root)) {
        if (entry.is_directory() &&
            !entry.path().filename().string().starts_with(".partial_"))
        {
            ++published;
        }
    }
    EXPECT_EQ(0u, published);
}

} // namespace
