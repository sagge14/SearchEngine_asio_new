#include "ServiceArchive.h"

#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

class ServiceArchiveTemporaryDirectory final {
public:
    ServiceArchiveTemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            (L"SearchEngineServiceArchiveTests-" + std::to_wstring(stamp));
        fs::create_directories(path_);
    }

    ~ServiceArchiveTemporaryDirectory()
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

void executeSql(sqlite3* database, const char* sql)
{
    char* rawError = nullptr;
    const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
    const std::string detail = rawError ? rawError : "";
    sqlite3_free(rawError);
    ASSERT_EQ(rc, SQLITE_OK) << detail;
}

json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    json value;
    input >> value;
    return value;
}

void writeJson(const fs::path& path, const json& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

void writeTextFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value;
    ASSERT_TRUE(output.good());
}

class RestoredArchiveDeletionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        serviceName_ = L"SearchEngineService-2026-prd";
        archiveDirectory_ = temporary_.path() / L"archive-root" /
            L"SearchEngineService-2026-prd-2026";
        siblingArchive_ = temporary_.path() / L"archive-root" /
            L"SearchEngineService-2026-prm-2026";
        originalExecutable_ = temporary_.path() / L"original" / L"program" /
            L"SearchEngine.exe";
        originalData_ = temporary_.path() / L"original" / L"data";
        archivedExecutable_ = archiveDirectory_ / L"program" / L"SearchEngine.exe";
        archivedData_ = archiveDirectory_ / L"data";

        writeTextFile(originalExecutable_, "original-exe");
        writeTextFile(originalData_ / L"Settings.json", "{}");
        writeTextFile(originalData_ / L"inverted_index.sqlite", "index");
        writeTextFile(archivedExecutable_, "archived-exe");
        writeTextFile(archivedData_ / L"Settings.json", "{}");
        writeTextFile(archivedData_ / L"inverted_index.sqlite", "index");
        writeTextFile(siblingArchive_ / L"sentinel.txt", "keep-sibling");

        originalImage_ = searchengine_archive::buildServiceImagePath(
            originalExecutable_, serviceName_, originalData_);
        archivedImage_ = searchengine_archive::buildServiceImagePath(
            archivedExecutable_, serviceName_, archivedData_);
        manifest_ = {
            {"format_version", 1},
            {"operation", "service-archive"},
            {"phase", "restored-running"},
            {"year", 2026},
            {"service_name", encoding::wstring_to_utf8(serviceName_)},
            {"original_image_path", encoding::wstring_to_utf8(originalImage_)},
            {"archived_image_path", encoding::wstring_to_utf8(archivedImage_)},
            {"original_executable", utf8(originalExecutable_)},
            {"original_data_directory", utf8(originalData_)},
            {"archived_executable", utf8(archivedExecutable_)},
            {"archived_data_directory", utf8(archivedData_)},
            {"mappings", json::array({
                {{"source", utf8(originalExecutable_.parent_path())},
                 {"target", utf8(archiveDirectory_ / L"program")}},
                {{"source", utf8(originalData_)},
                 {"target", utf8(archivedData_)}}})},
            {"files", json::array()},
            {"monthly_databases", json::array()}};
        saveManifest();

        selected_.serviceName = serviceName_;
        selected_.imagePath = originalImage_;
        selected_.executable = originalExecutable_;
        selected_.dataDirectory = originalData_;
        selected_.currentState = SERVICE_RUNNING;
    }

    void saveManifest()
    {
        writeJson(archiveDirectory_ / L"archive-operation.json", manifest_);
    }

    ServiceArchiveTemporaryDirectory temporary_;
    std::wstring serviceName_;
    fs::path archiveDirectory_;
    fs::path siblingArchive_;
    fs::path originalExecutable_;
    fs::path originalData_;
    fs::path archivedExecutable_;
    fs::path archivedData_;
    std::wstring originalImage_;
    std::wstring archivedImage_;
    json manifest_;
    searchengine_archive::InstalledService selected_;
};

} // namespace

#ifdef _WIN32
TEST(ServiceArchiveInvocation, RoundTripsQuotedServiceImagePath)
{
    const fs::path executable =
        L"C:\\Program Files\\Search Engine\\SearchEngine.exe";
    const fs::path dataDirectory = L"D:\\Search Data\\2026";
    const std::wstring imagePath = searchengine_archive::buildServiceImagePath(
        executable, L"SearchEngineService-2026", dataDirectory);

    const auto parsed = searchengine_archive::parseServiceInvocation(imagePath);
    EXPECT_EQ(parsed.executable, executable.lexically_normal());
    EXPECT_EQ(parsed.dataDirectory, dataDirectory.lexically_normal());
    EXPECT_EQ(parsed.serviceNameArgument, L"SearchEngineService-2026");
}
#endif

TEST(ServiceArchiveCatalog, RewritesOnlyDocumentPathsAndPreservesIdentifiers)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path databasePath = temporary.path() / L"inverted_index.sqlite";
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(utf8(databasePath).c_str(), &database), SQLITE_OK);
    ASSERT_NE(database, nullptr);
    executeSql(
        database,
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE docs ("
        "doc_id INTEGER PRIMARY KEY, path TEXT NOT NULL, "
        "mtime_ticks INTEGER NOT NULL, size_int64 INTEGER NOT NULL, "
        "deleted INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE postings (token TEXT NOT NULL, doc_id INTEGER NOT NULL);"
        "INSERT INTO docs VALUES (7,'D:\\SOURCE\\2026\\file.txt',101,202,0);"
        "INSERT INTO postings VALUES ('needle',7);");
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);

    const std::vector<searchengine_archive::PathMapping> mappings{
        {L"D:\\SOURCE", L"E:\\ARCHIVE\\content"}};
    ASSERT_NO_THROW(searchengine_archive::rewriteDocumentCatalogPaths(
        databasePath, mappings));

    ASSERT_EQ(sqlite3_open_v2(
        utf8(databasePath).c_str(), &database,
        SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        database,
        "SELECT d.doc_id,d.path,d.mtime_ticks,d.size_int64,d.deleted,p.token "
        "FROM docs d JOIN postings p ON p.doc_id=d.doc_id",
        -1, &statement, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(statement, 0), 7);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        "E:\\ARCHIVE\\content\\2026\\file.txt");
    EXPECT_EQ(sqlite3_column_int64(statement, 2), 101);
    EXPECT_EQ(sqlite3_column_int64(statement, 3), 202);
    EXPECT_EQ(sqlite3_column_int(statement, 4), 0);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 5)),
        "needle");
    sqlite3_finalize(statement);
    EXPECT_EQ(sqlite3_close(database), SQLITE_OK);
}

TEST(ServiceArchiveSettings, FreezesAndRestoresExplicitMonthlyDirectories)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path settingsPath = temporary.path() / L"Settings.json";
    writeJson(
        settingsPath,
        {{"config", {
            {"year", "2026"},
            {"server_mode", "active"},
            {"document_catalog_storage", "sqlite"},
            {"scan_on_startup", true},
            {"index_roots", json::array({"D:\\SOURCE\\2026"})},
            {"excluded_subtrees", json::array({"D:\\SOURCE\\2026\\TEMP"})},
            {"prm_base_dir", "D:\\BASES"},
            {"prd_base_dir", "D:\\BASES_PRD"},
            {"prm_monthly_bases_dir", "D:\\BASES\\METH_BASES"},
            {"prd_monthly_bases_dir", "D:\\BASES_PRD\\METH_BASES"}}}});

    const std::vector<searchengine_archive::PathMapping> forward{
        {L"D:\\SOURCE", L"E:\\ARCHIVE\\content"}};
    searchengine_archive::rewriteSettingsForArchive(
        settingsPath,
        forward,
        L"E:\\ARCHIVE\\autopad\\PRM\\monthly",
        L"E:\\ARCHIVE\\autopad\\PRD\\monthly");

    json settings = readJson(settingsPath);
    const json& archived = settings.at("config");
    EXPECT_EQ(archived.at("server_mode"), "archive");
    EXPECT_EQ(archived.at("document_catalog_storage"), "sqlite");
    EXPECT_FALSE(archived.at("scan_on_startup").get<bool>());
    EXPECT_EQ(archived.at("index_roots").at(0),
              "E:\\ARCHIVE\\content\\2026");
    EXPECT_EQ(archived.at("prm_monthly_bases_dir"),
              "E:\\ARCHIVE\\autopad\\PRM\\monthly");
    EXPECT_EQ(archived.at("prd_monthly_bases_dir"),
              "E:\\ARCHIVE\\autopad\\PRD\\monthly");
    EXPECT_EQ(archived.at("prm_base_dir"), "");
    EXPECT_EQ(archived.at("prd_base_dir"), "");

    const std::vector<searchengine_archive::PathMapping> reverse{
        {L"E:\\ARCHIVE\\content", L"D:\\SOURCE"}};
    searchengine_archive::rewriteSettingsForActive(
        settingsPath,
        reverse,
        L"D:\\BASES\\METH_BASES",
        L"D:\\BASES_PRD\\METH_BASES");

    settings = readJson(settingsPath);
    const json& active = settings.at("config");
    EXPECT_EQ(active.at("server_mode"), "active");
    EXPECT_EQ(active.at("index_roots").at(0), "D:\\SOURCE\\2026");
    EXPECT_EQ(active.at("prm_monthly_bases_dir"),
              "D:\\BASES\\METH_BASES");
    EXPECT_EQ(active.at("prd_monthly_bases_dir"),
              "D:\\BASES_PRD\\METH_BASES");
    EXPECT_EQ(active.at("prm_base_dir"), "D:\\BASES");
    EXPECT_EQ(active.at("prd_base_dir"), "D:\\BASES_PRD");
}

TEST_F(RestoredArchiveDeletionTest, ValidRestoredArchivePassesWithoutScmAccess)
{
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {selected_});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(fs::exists(archiveDirectory_));
}

TEST_F(RestoredArchiveDeletionTest, RejectsWrongPhaseAndPreservesArchive)
{
    manifest_["phase"] = "archive-running";
    saveManifest();
    const auto result =
        searchengine_archive::deleteRestoredServiceArchive(
            archiveDirectory_, {selected_});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(archiveDirectory_));
    EXPECT_TRUE(fs::exists(siblingArchive_ / L"sentinel.txt"));
}

TEST_F(RestoredArchiveDeletionTest, RejectsImagePathMismatch)
{
    auto mismatched = selected_;
    mismatched.imagePath = L"C:\\wrong\\SearchEngine.exe --service";
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {mismatched});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(archiveDirectory_));
}

TEST_F(RestoredArchiveDeletionTest, RejectsStoppedService)
{
    auto stopped = selected_;
    stopped.currentState = SERVICE_STOPPED;
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {stopped});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(archiveDirectory_));
}

TEST_F(RestoredArchiveDeletionTest, RejectsWhenAnotherServiceUsesArchive)
{
    searchengine_archive::InstalledService other;
    other.serviceName = L"SearchEngineService-other";
    other.executable = archivedExecutable_;
    other.dataDirectory = archivedData_;
    other.imagePath = searchengine_archive::buildServiceImagePath(
        other.executable, other.serviceName, other.dataDirectory);
    other.currentState = SERVICE_RUNNING;

    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {selected_, other});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(archiveDirectory_));
    EXPECT_TRUE(fs::exists(siblingArchive_ / L"sentinel.txt"));
}

TEST_F(RestoredArchiveDeletionTest, RejectsManifestPathOutsideArchive)
{
    manifest_["mappings"][0]["target"] = utf8(
        temporary_.path() / L"escape" / L"program");
    saveManifest();
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {selected_});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(archiveDirectory_));
}

TEST_F(RestoredArchiveDeletionTest, DeletesOnlyExactValidatedServiceArchive)
{
    const auto result = searchengine_archive::deleteRestoredServiceArchive(
        archiveDirectory_, {selected_});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(fs::exists(archiveDirectory_));
    EXPECT_TRUE(fs::exists(siblingArchive_ / L"sentinel.txt"));
    EXPECT_TRUE(fs::exists(originalExecutable_));
    EXPECT_TRUE(fs::exists(originalData_ / L"Settings.json"));
}
