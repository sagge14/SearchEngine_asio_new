#include "Backup/FileHash.h"
#include "Backup/Restore/MirrorHistoryStore.h"
#include "Backup/Restore/RestoreInterfaces.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

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

class TemporaryRestoreDirectory {
public:
    TemporaryRestoreDirectory()
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            fs::temp_directory_path() /
            (
                "searchengine_backup_restore_test_" +
                std::to_string(unique) + "_" +
                std::to_string(sequence.fetch_add(1))
            );
        fs::create_directories(path_);
    }

    ~TemporaryRestoreDirectory()
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

void writeFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << value;
}

std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

void writeManifest(const fs::path& path, const nh::json& root)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << root.dump(2);
}

struct FixtureHashes {
    FileHashResult hash_a;
    FileHashResult hash_b;
    FileHashResult hash_keep;
};

FixtureHashes buildFixture(const fs::path& root, bool include_object_a = true)
{
    const fs::path target = root / "BASES_0123456789abcdef";
    const std::string content_a = "version-A-content";
    const std::string content_b = "version-B-content-longer";

    writeFile(target / "current" / "data" / "ARCHIVE.db3", content_b);
    writeFile(target / "current" / "data" / "keep.txt", "stable");

    const fs::path temp_a = root / "temp_a.bin";
    writeFile(temp_a, content_a);
    FixtureHashes hashes;
    hashes.hash_a = sha256File(temp_a);
    hashes.hash_b = sha256File(target / "current" / "data" / "ARCHIVE.db3");
    hashes.hash_keep = sha256File(target / "current" / "data" / "keep.txt");
    if (!hashes.hash_a.ok || !hashes.hash_b.ok || !hashes.hash_keep.ok) {
        throw std::runtime_error("fixture hashing failed");
    }

    if (include_object_a) {
        writeFile(
            target / "objects" / "by-path" / "ARCHIVE.db3" /
                hashes.hash_a.sha256,
            content_a
        );
    }

    writeManifest(
        target / "current" / "manifest.json",
        nh::json{
            {"format", 1},
            {"strategy", "mirror_history"},
            {"source", "D:/BASES"},
            {"updated_at", "20260101_120000"},
            {"updated_unix_seconds", 1735732800},
            {"complete", true},
            {"directories", nh::json::array({"subdir"})},
            {"errors", nh::json::array()},
            {"files", nh::json::array({
                {
                    {"path", "ARCHIVE.db3"},
                    {"size", hashes.hash_b.size},
                    {"sha256", hashes.hash_b.sha256},
                    {"captured_at", "20260101_120000"},
                    {"method", "filesystem"}
                },
                {
                    {"path", "keep.txt"},
                    {"size", hashes.hash_keep.size},
                    {"sha256", hashes.hash_keep.sha256},
                    {"captured_at", "20260101_120000"},
                    {"method", "filesystem"}
                }
            })}
        }
    );

    writeManifest(
        target / "restore_points" / "every_3min" / "20260101_115700" /
            "manifest.json",
        nh::json{
            {"format", 1},
            {"strategy", "mirror_history"},
            {"source", "D:/BASES"},
            {"updated_at", "20260101_115700"},
            {"updated_unix_seconds", 1735732620},
            {"point_tier", "every_3min"},
            {"point_created_at", "20260101_115700"},
            {"point_created_unix_seconds", 1735732620},
            {"complete", true},
            {"directories", nh::json::array()},
            {"errors", nh::json::array()},
            {"files", nh::json::array({
                {
                    {"path", "ARCHIVE.db3"},
                    {"size", hashes.hash_a.size},
                    {"sha256", hashes.hash_a.sha256},
                    {"captured_at", "20260101_115700"},
                    {"method", "filesystem"}
                },
                {
                    {"path", "keep.txt"},
                    {"size", hashes.hash_keep.size},
                    {"sha256", hashes.hash_keep.sha256},
                    {"captured_at", "20260101_115700"},
                    {"method", "filesystem"}
                }
            })}
        }
    );

    return hashes;
}

RestorePointInfo historicalPoint(
    RestoreServices& services,
    const RestoreTargetInfo& target)
{
    std::string error;
    const auto points = services.points->listPoints(target, error);
    EXPECT_TRUE(error.empty()) << error;
    for (const auto& point : points) {
        if (!point.is_current) {
            return point;
        }
    }
    ADD_FAILURE() << "historical point not found";
    return {};
}

} // namespace

TEST(BackupRestore, ScansPointsFilesPlanVerifyAndRestore)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;

    const auto targets = services.scanner->scanRoot(backup_root, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(1u, targets.size());
    EXPECT_EQ("BASES", targets.front().display_name);
    EXPECT_TRUE(targets.front().has_current);
    EXPECT_EQ(2u, targets.front().file_count_current);

    const auto points =
        services.points->listPoints(targets.front(), error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_GE(points.size(), 2u);
    EXPECT_TRUE(points.front().is_current);
    EXPECT_EQ("current", points.front().tier);

    const RestorePointInfo historical =
        historicalPoint(services, targets.front());
    ASSERT_FALSE(historical.manifest_path.empty());
    EXPECT_EQ("every_3min", historical.tier);
    EXPECT_EQ(
        targets.front().root_path,
        historical.target_root
    );

    const auto files = services.files->listFiles(historical, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(2u, files.size());

    bool archive_from_objects = false;
    bool keep_from_current = false;
    for (const auto& file : files) {
        if (file.relative_path == "ARCHIVE.db3") {
            EXPECT_EQ(
                RestoreResolveStatus::InObjects,
                file.resolve_status
            );
            archive_from_objects = true;
        }
        if (file.relative_path == "keep.txt") {
            EXPECT_EQ(
                RestoreResolveStatus::InCurrent,
                file.resolve_status
            );
            keep_from_current = true;
        }
    }
    EXPECT_TRUE(archive_from_objects);
    EXPECT_TRUE(keep_from_current);

    const auto plan = services.planner->plan(historical, {}, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(2u, plan.size());

    ASSERT_TRUE(
        services.verifier->verify(historical, {}, nullptr, error)
    ) << error;

    const fs::path restored = temporary.path() / "restored";
    RestoreRequest request;
    request.point = historical;
    request.destination = restored;
    ASSERT_TRUE(
        services.executor->restore(request, nullptr, error)
    ) << error;
    EXPECT_EQ("version-A-content", readFile(restored / "ARCHIVE.db3"));
    EXPECT_EQ("stable", readFile(restored / "keep.txt"));

    RestorePointInfo current =
        services.points->currentPoint(targets.front(), error);
    ASSERT_TRUE(error.empty()) << error;
    const fs::path restored_current =
        temporary.path() / "restored_current";
    request.point = current;
    request.destination = restored_current;
    ASSERT_TRUE(
        services.executor->restore(request, nullptr, error)
    ) << error;
    EXPECT_EQ(
        "version-B-content-longer",
        readFile(restored_current / "ARCHIVE.db3")
    );
}

TEST(BackupRestore, LoadPointFromManifestResolvesTargetRoot)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    const auto targets = services.scanner->scanRoot(backup_root, error);
    ASSERT_EQ(1u, targets.size());

    const fs::path hist_manifest =
        targets.front().root_path / "restore_points" / "every_3min" /
        "20260101_115700" / "manifest.json";

    RestorePointInfo point;
    ASSERT_TRUE(
        services.points->loadPointFromManifest(hist_manifest, point, error)
    ) << error;
    EXPECT_EQ(targets.front().root_path, point.target_root);
    EXPECT_FALSE(point.is_current);
    EXPECT_EQ("every_3min", point.tier);

    ASSERT_TRUE(
        services.verifier->verify(point, {}, nullptr, error)
    ) << error;
}

TEST(BackupRestore, PathFilterRestoresOnlySelectedFiles)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    const auto targets = services.scanner->scanRoot(backup_root, error);
    ASSERT_EQ(1u, targets.size());
    const RestorePointInfo historical =
        historicalPoint(services, targets.front());

    const auto plan = services.planner->plan(
        historical,
        {"ARCHIVE.db3"},
        error
    );
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ("ARCHIVE.db3", plan.front().path);

    RestoreRequest request;
    request.point = historical;
    request.destination = temporary.path() / "filtered";
    request.path_filter = {"ARCHIVE.db3"};
    ASSERT_TRUE(
        services.executor->restore(request, nullptr, error)
    ) << error;
    EXPECT_EQ(
        "version-A-content",
        readFile(request.destination / "ARCHIVE.db3")
    );
    EXPECT_FALSE(fs::exists(request.destination / "keep.txt"));
}

TEST(BackupRestore, MissingObjectFailsVerifyAndRestore)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root, false);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    const auto targets = services.scanner->scanRoot(backup_root, error);
    ASSERT_EQ(1u, targets.size());
    // Without the historical object, current still has a different ARCHIVE.
    // Remove it so the historical sha cannot resolve at all.
    ASSERT_TRUE(fs::remove(
        targets.front().root_path / "current" / "data" / "ARCHIVE.db3"
    ));

    const RestorePointInfo historical =
        historicalPoint(services, targets.front());

    const auto files = services.files->listFiles(historical, error);
    ASSERT_TRUE(error.empty()) << error;
    bool archive_missing = false;
    for (const auto& file : files) {
        if (file.relative_path == "ARCHIVE.db3") {
            EXPECT_EQ(RestoreResolveStatus::Missing, file.resolve_status);
            archive_missing = true;
        }
    }
    EXPECT_TRUE(archive_missing);

    EXPECT_FALSE(
        services.verifier->verify(historical, {}, nullptr, error)
    );
    EXPECT_FALSE(error.empty());

    RestoreRequest request;
    request.point = historical;
    request.destination = temporary.path() / "should_fail";
    error.clear();
    EXPECT_FALSE(
        services.executor->restore(request, nullptr, error)
    );
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(request.destination));
}

TEST(BackupRestore, OverwriteRequiresFlagAndRetiresOldDestination)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    const auto targets = services.scanner->scanRoot(backup_root, error);
    ASSERT_EQ(1u, targets.size());
    RestorePointInfo current =
        services.points->currentPoint(targets.front(), error);
    ASSERT_TRUE(error.empty()) << error;

    const fs::path destination = temporary.path() / "dest";
    writeFile(destination / "old.txt", "preexisting");

    RestoreRequest request;
    request.point = current;
    request.destination = destination;
    request.overwrite = false;
    EXPECT_FALSE(
        services.executor->restore(request, nullptr, error)
    );
    EXPECT_TRUE(fs::exists(destination / "old.txt"));

    request.overwrite = true;
    error.clear();
    ASSERT_TRUE(
        services.executor->restore(request, nullptr, error)
    ) << error;
    EXPECT_EQ(
        "version-B-content-longer",
        readFile(destination / "ARCHIVE.db3")
    );
    EXPECT_FALSE(fs::exists(destination / "old.txt"));

    bool retired_found = false;
    for (const auto& entry : fs::directory_iterator(temporary.path())) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("dest.before_restore", 0) == 0) {
            retired_found = true;
            EXPECT_TRUE(fs::exists(entry.path() / "old.txt"));
        }
    }
    EXPECT_TRUE(retired_found);
}

TEST(BackupRestore, RejectsUnsafeRelativePathInManifest)
{
    TemporaryRestoreDirectory temporary;
    const fs::path manifest =
        temporary.path() / "bad" / "current" / "manifest.json";
    writeManifest(
        manifest,
        nh::json{
            {"format", 1},
            {"strategy", "mirror_history"},
            {"source", "D:/BASES"},
            {"updated_at", "20260101_120000"},
            {"updated_unix_seconds", 1},
            {"complete", true},
            {"directories", nh::json::array()},
            {"errors", nh::json::array()},
            {"files", nh::json::array({
                {
                    {"path", "../escape.txt"},
                    {"size", 1},
                    {"sha256", std::string(64, 'a')},
                    {"captured_at", "20260101_120000"},
                    {"method", "filesystem"}
                }
            })}
        }
    );

    const MirrorManifest parsed = readMirrorManifest(manifest);
    EXPECT_FALSE(parsed.ok);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(BackupRestore, FindTargetByDisplayNameAndId)
{
    TemporaryRestoreDirectory temporary;
    const fs::path backup_root = temporary.path() / "store";
    fs::create_directories(backup_root);
    buildFixture(backup_root);

    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    RestoreTargetInfo target;
    ASSERT_TRUE(
        services.points->findTarget(backup_root, "BASES", target, error)
    ) << error;
    EXPECT_EQ("BASES_0123456789abcdef", target.id);

    RestoreTargetInfo by_id;
    ASSERT_TRUE(
        services.points->findTarget(
            backup_root,
            "BASES_0123456789abcdef",
            by_id,
            error
        )
    ) << error;
    EXPECT_EQ(target.root_path, by_id.root_path);
}
