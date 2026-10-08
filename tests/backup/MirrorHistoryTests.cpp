#include "Backup/BackupEngine.h"
#include "Backup/FileHash.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace nh = nlohmann;

class TemporaryMirrorDirectory {
public:
    TemporaryMirrorDirectory()
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            fs::temp_directory_path() /
            (
                "searchengine_mirror_history_test_" +
                std::to_string(unique) + "_" +
                std::to_string(sequence.fetch_add(1))
            );
        fs::create_directories(path_);
    }

    ~TemporaryMirrorDirectory()
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

void writeMirrorFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << value;
}

std::string readMirrorFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

nh::json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    nh::json result;
    input >> result;
    return result;
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

size_t pointCount(const fs::path& target_root,
                  const std::string& period)
{
    const fs::path root =
        target_root / "restore_points" / period;
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

std::vector<fs::path> objectFiles(const fs::path& target_root)
{
    std::vector<fs::path> files;
    const fs::path root = target_root / "objects" / "by-path";
    std::error_code error;
    if (!fs::exists(root, error) || error) {
        return files;
    }
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end;
         it.increment(error))
    {
        if (it->is_regular_file(error)) {
            files.push_back(it->path());
        }
    }
    return files;
}

BackupTarget mirrorTarget(const fs::path& source)
{
    BackupTarget target;
    target.src = source;
    target.is_directory = true;
    target.mode = BackupMode::Filesystem;
    target.strategy = BackupStrategy::MirrorHistory;
    target.history_tiers = {
        BackupHistoryTier{"every_10s", 10, 2},
        BackupHistoryTier{"every_1h", 3600, 1}
    };
    return target;
}

const auto mirror_start_time =
    std::chrono::system_clock::from_time_t(1704067200);

TEST(MirrorHistory, MaintainsCurrentMirrorAndOnlyUniqueOldVersions)
{
    TemporaryMirrorDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup = temporary.path() / "backup";
    writeMirrorFile(source / "active.txt", "version A");
    writeMirrorFile(source / "monthly" / "old.txt", "never changes");

    BackupEngine engine(backup, {mirrorTarget(source)});
    const BackupRunResult first = engine.runOnceAt(mirror_start_time);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(1u, first.targets.size());
    EXPECT_TRUE(
        first.targets.front().status ==
        BackupTargetStatus::MirrorUpdated
    );

    const fs::path target_root = onlyTargetRoot(backup);
    const fs::path current = target_root / "current";
    EXPECT_EQ(
        "version A",
        readMirrorFile(current / "data" / "active.txt")
    );
    EXPECT_EQ(
        "never changes",
        readMirrorFile(current / "data" / "monthly" / "old.txt")
    );
    const nh::json initial_manifest =
        readJson(current / "manifest.json");
    EXPECT_TRUE(initial_manifest.at("complete").get<bool>());
    EXPECT_EQ(2u, initial_manifest.at("files").size());
    EXPECT_EQ(1u, pointCount(target_root, "every_10s"));
    EXPECT_EQ(1u, pointCount(target_root, "every_1h"));
    EXPECT_TRUE(objectFiles(target_root).empty());

    const BackupRunResult unchanged = engine.runOnceAt(
        mirror_start_time + std::chrono::seconds(5)
    );
    ASSERT_TRUE(unchanged.ok());
    EXPECT_TRUE(
        unchanged.targets.front().status ==
        BackupTargetStatus::Unchanged
    );
    EXPECT_TRUE(objectFiles(target_root).empty());

    writeMirrorFile(source / "active.txt", "version B");
    const BackupRunResult changed = engine.runOnceAt(
        mirror_start_time + std::chrono::seconds(10)
    );
    ASSERT_TRUE(changed.ok());
    EXPECT_EQ(
        "version B",
        readMirrorFile(current / "data" / "active.txt")
    );
    EXPECT_EQ(2u, pointCount(target_root, "every_10s"));
    EXPECT_EQ(1u, pointCount(target_root, "every_1h"));

    const std::vector<fs::path> objects = objectFiles(target_root);
    ASSERT_EQ(1u, objects.size());
    EXPECT_EQ("version A", readMirrorFile(objects.front()));
    EXPECT_EQ(
        sha256String("version A"),
        objects.front().filename().string()
    );
}

TEST(MirrorHistory, ArchivesDeletionAndRotatesManifestOnlyPoints)
{
    TemporaryMirrorDirectory temporary;
    const fs::path source = temporary.path() / "source";
    const fs::path backup = temporary.path() / "backup";
    writeMirrorFile(source / "active.txt", "A");
    writeMirrorFile(source / "removed.txt", "historical");

    BackupEngine engine(backup, {mirrorTarget(source)});
    ASSERT_TRUE(engine.runOnceAt(mirror_start_time).ok());
    const fs::path target_root = onlyTargetRoot(backup);

    fs::remove(source / "removed.txt");
    writeMirrorFile(source / "active.txt", "B");
    ASSERT_TRUE(
        engine.runOnceAt(
            mirror_start_time + std::chrono::seconds(10)
        ).ok()
    );
    EXPECT_FALSE(
        fs::exists(target_root / "current" / "data" / "removed.txt")
    );

    writeMirrorFile(source / "active.txt", "C");
    ASSERT_TRUE(
        engine.runOnceAt(
            mirror_start_time + std::chrono::seconds(20)
        ).ok()
    );
    EXPECT_EQ(2u, pointCount(target_root, "every_10s"));

    bool found_removed = false;
    for (const fs::path& object : objectFiles(target_root)) {
        if (readMirrorFile(object) == "historical") {
            found_removed = true;
        }
    }
    EXPECT_TRUE(found_removed);
}

} // namespace
