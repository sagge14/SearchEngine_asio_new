#include "ArchiveCore.h"

#include "Backup/FileHash.h"
#include "MyUtils/Encoding.h"
#include "sqlite3.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using searchengine_archive::MonthlyDatabase;
using searchengine_archive::YearMoveOptions;

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            (L"SearchEngineArchiveTests-" + std::to_wstring(stamp));
        fs::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string utf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

void execute(sqlite3* database, const std::string& sql)
{
    char* error = nullptr;
    const int rc = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    const std::string detail = error ? error : "";
    sqlite3_free(error);
    ASSERT_EQ(rc, SQLITE_OK) << detail;
}

void createMonthlyDatabase(
    const fs::path& path,
    const fs::path& directTo,
    const std::string& fileName,
    bool withoutRowId = false)
{
    fs::create_directories(path.parent_path());
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(utf8(path).c_str(), &database), SQLITE_OK);
    ASSERT_NE(database, nullptr);
    std::string schema =
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE archive ("
        "`index` INTEGER PRIMARY KEY, DirectTo TEXT, FileName TEXT)";
    schema += withoutRowId ? " WITHOUT ROWID;" : ";";
    execute(database, schema);

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            database,
            "INSERT INTO archive (`index`, DirectTo, FileName) VALUES (1, ?, ?)",
            -1, &statement, nullptr),
        SQLITE_OK);
    const std::string directory = utf8(directTo) + "\\";
    ASSERT_EQ(
        sqlite3_bind_text(statement, 1, directory.c_str(), -1, SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_bind_text(statement, 2, fileName.c_str(), -1, SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
}

std::pair<std::string, std::string> readPathFields(const fs::path& path)
{
    sqlite3* database = nullptr;
    EXPECT_EQ(sqlite3_open_v2(
        utf8(path).c_str(), &database, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        database, "SELECT DirectTo, FileName FROM archive", -1,
        &statement, nullptr), SQLITE_OK);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto* directTo = sqlite3_column_text(statement, 0);
    const auto* fileName = sqlite3_column_text(statement, 1);
    std::pair<std::string, std::string> result{
        directTo ? reinterpret_cast<const char*>(directTo) : "",
        fileName ? reinterpret_cast<const char*>(fileName) : ""};
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

void writeText(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value;
    ASSERT_TRUE(output.good());
}

} // namespace

TEST(ArchivePathContract, UsesLongestComponentBoundedMapping)
{
    const std::vector<searchengine_archive::PathMapping> mappings{
        {fs::path(L"C:\\DATA"), fs::path(L"E:\\ARCHIVE\\DATA")},
        {fs::path(L"C:\\DATA\\SPECIAL"), fs::path(L"E:\\ARCHIVE\\SPECIAL")}};

    EXPECT_EQ(
        searchengine_archive::rebasePath(
            fs::path(L"C:\\DATA\\SPECIAL\\file.txt"), mappings),
        fs::path(L"E:\\ARCHIVE\\SPECIAL\\file.txt"));
    EXPECT_FALSE(searchengine_archive::isPathEqualOrBelow(
        fs::path(L"C:\\DATABASE\\file.txt"), fs::path(L"C:\\DATA")));
}

TEST(ArchivePathContract, KeepsOnlySelectedTlgYearAndAlwaysSkipsOut)
{
    const auto skipDirectory = [](const fs::path& name) {
        return searchengine_archive::shouldSkipTlgTopLevelEntry(
            name, true, 2026);
    };
    EXPECT_FALSE(skipDirectory(L"2026"));
    EXPECT_TRUE(skipDirectory(L"2025"));
    EXPECT_TRUE(skipDirectory(L"2027"));
    EXPECT_TRUE(skipDirectory(L"OUT"));
    EXPECT_TRUE(skipDirectory(L"out"));
    EXPECT_TRUE(skipDirectory(L"Out"));
    EXPECT_FALSE(skipDirectory(L"08"));
    EXPECT_FALSE(skipDirectory(L"TEMP"));
    EXPECT_FALSE(skipDirectory(L"02027"));
    EXPECT_FALSE(searchengine_archive::shouldSkipTlgTopLevelEntry(
        L"2027", false, 2026));

    EXPECT_TRUE(searchengine_archive::shouldSkipArchiveTreeEntry(
        L"D:\\TLG", L"2025", 0, true, 2026));
    EXPECT_FALSE(searchengine_archive::shouldSkipArchiveTreeEntry(
        L"D:\\TLG", L"2026", 0, true, 2026));
    EXPECT_FALSE(searchengine_archive::shouldSkipArchiveTreeEntry(
        L"D:\\TLG", L"2027", 0, false, 2026));
    EXPECT_FALSE(searchengine_archive::shouldSkipArchiveTreeEntry(
        L"D:\\TLG", L"2027", 1, true, 2026));
    EXPECT_FALSE(searchengine_archive::shouldSkipArchiveTreeEntry(
        L"D:\\OTHER", L"2027", 0, true, 2026));
}

TEST(ArchiveYearMove, CopiesAndRewritesOnlyTheStagedMonthlyDatabases)
{
    TemporaryDirectory temporary;
    const fs::path sourceFiles = temporary.path() / L"source-files";
    const fs::path prmDirectory = temporary.path() / L"prm-monthly";
    const fs::path prdDirectory = temporary.path() / L"prd-monthly";
    const fs::path targetRoot = temporary.path() / L"archive";
    const fs::path payload = sourceFiles / L"message.txt";
    writeText(payload, "archive-payload");

    const fs::path prmDatabase = prmDirectory / L"01-2026.db3";
    const fs::path prdDatabase = prdDirectory / L"12-2026.db3";
    const fs::path livePrmArchive = prmDirectory / L"ARCHIVE.db3";
    const fs::path livePrdArchive = prdDirectory / L"ARCHIVE.db3";
    writeText(livePrmArchive, "live-prm-must-not-move");
    writeText(livePrdArchive, "live-prd-must-not-move");
    createMonthlyDatabase(prmDatabase, sourceFiles, "message.txt");
    createMonthlyDatabase(prdDatabase, sourceFiles, "message.txt", true);
    const FileHashResult prmBefore = sha256File(prmDatabase);
    const FileHashResult prdBefore = sha256File(prdDatabase);
    ASSERT_TRUE(prmBefore.ok);
    ASSERT_TRUE(prdBefore.ok);

    YearMoveOptions options;
    options.year = 2026;
    options.prmMonthlyDirectory = prmDirectory;
    options.prdMonthlyDirectory = prdDirectory;
    options.archiveRoot = targetRoot;

    const auto plan = searchengine_archive::planYearMove(options);
    ASSERT_EQ(plan.mappings.size(), 1u);
    EXPECT_EQ(plan.mappings.front().target.filename(), L"source-files");
    ASSERT_EQ(plan.databases.size(), 2u);
    const auto result = searchengine_archive::executeYearMove(plan);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(fs::exists(result.manifestPath));
    EXPECT_TRUE(fs::exists(plan.mappings.front().target / L"message.txt"));
    EXPECT_TRUE(fs::exists(payload));
    EXPECT_FALSE(fs::exists(result.finalDirectory / L"autopad" / L"PRM" / L"ARCHIVE.db3"));
    EXPECT_FALSE(fs::exists(result.finalDirectory / L"autopad" / L"PRD" / L"ARCHIVE.db3"));
    EXPECT_TRUE(fs::exists(livePrmArchive));
    EXPECT_TRUE(fs::exists(livePrdArchive));

    const FileHashResult prmAfter = sha256File(prmDatabase);
    const FileHashResult prdAfter = sha256File(prdDatabase);
    ASSERT_TRUE(prmAfter.ok);
    ASSERT_TRUE(prdAfter.ok);
    EXPECT_EQ(prmAfter.sha256, prmBefore.sha256);
    EXPECT_EQ(prdAfter.sha256, prdBefore.sha256);

    const auto prmFields = readPathFields(
        result.finalDirectory / L"autopad" / L"PRM" / L"monthly" /
        L"01-2026.db3");
    EXPECT_EQ(prmFields.second, "message.txt");
    EXPECT_TRUE(fs::equivalent(
        fs::path(encoding::utf8_to_wstring(prmFields.first)),
        plan.mappings.front().target));

    const auto prdFields = readPathFields(
        result.finalDirectory / L"autopad" / L"PRD" / L"monthly" /
        L"12-2026.db3");
    EXPECT_EQ(prdFields.second, "message.txt");
    EXPECT_TRUE(fs::equivalent(
        fs::path(encoding::utf8_to_wstring(prdFields.first)),
        plan.mappings.front().target));

    const auto cleanup = searchengine_archive::cleanupYearMoveFiles(
        result.finalDirectory, false);
    ASSERT_TRUE(cleanup.ok) << cleanup.message;
    EXPECT_FALSE(fs::exists(payload));
    EXPECT_TRUE(fs::exists(prmDatabase));
    EXPECT_TRUE(fs::exists(prdDatabase));

    const auto databaseCleanup = searchengine_archive::cleanupYearMoveFiles(
        result.finalDirectory, true);
    ASSERT_TRUE(databaseCleanup.ok) << databaseCleanup.message;
    EXPECT_FALSE(fs::exists(prmDatabase));
    EXPECT_TRUE(fs::exists(prdDatabase));
}

TEST(ArchiveYearMove, MissingMonthlyDatabasesProduceWarningsAndDoNotBlockMove)
{
    TemporaryDirectory temporary;
    const fs::path sourceFiles = temporary.path() / L"source-files";
    const fs::path prmDirectory = temporary.path() / L"prm-monthly";
    const fs::path prdDirectory = temporary.path() / L"prd-monthly";
    const fs::path targetRoot = temporary.path() / L"archive";
    writeText(sourceFiles / L"message.txt", "partial-monthly-set");

    createMonthlyDatabase(
        prmDirectory / L"01-2026.db3",
        sourceFiles,
        "message.txt");
    fs::create_directories(prdDirectory);

    YearMoveOptions options;
    options.year = 2026;
    options.prmMonthlyDirectory = prmDirectory;
    options.prdMonthlyDirectory = prdDirectory;
    options.archiveRoot = targetRoot;

    const auto plan = searchengine_archive::planYearMove(options);
    ASSERT_EQ(plan.databases.size(), 1u);
    ASSERT_EQ(plan.mappings.size(), 1u);
    EXPECT_FALSE(plan.warnings.empty());
    EXPECT_TRUE(std::any_of(
        plan.warnings.begin(), plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("PRD") != std::string::npos;
        }));

    const auto result = searchengine_archive::executeYearMove(plan);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(fs::exists(
        result.finalDirectory / L"autopad" / L"PRM" / L"monthly" /
        L"01-2026.db3"));
}

TEST(ArchiveYearMove, NoMonthlyDatabasesStillPublishesWarningManifest)
{
    TemporaryDirectory temporary;
    const fs::path prmDirectory = temporary.path() / L"prm-monthly";
    const fs::path targetRoot = temporary.path() / L"archive";
    fs::create_directories(prmDirectory);

    YearMoveOptions options;
    options.year = 2026;
    options.prmMonthlyDirectory = prmDirectory;
    options.prdMonthlyDirectory = temporary.path() / L"missing-prd-directory";
    options.archiveRoot = targetRoot;

    const auto plan = searchengine_archive::planYearMove(options);
    EXPECT_TRUE(plan.databases.empty());
    EXPECT_TRUE(plan.mappings.empty());
    EXPECT_GE(plan.warnings.size(), 2u);

    const auto result = searchengine_archive::executeYearMove(plan);
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_TRUE(fs::exists(result.manifestPath));
}
