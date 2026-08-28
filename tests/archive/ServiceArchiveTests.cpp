#include "ServiceArchive.h"

#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

std::string readTextFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void createAutoPadDatabase(
    const fs::path& path,
    const std::string& directTo,
    const std::string& fileName)
{
    fs::create_directories(path.parent_path());
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(utf8(path).c_str(), &database), SQLITE_OK);
    ASSERT_NE(database, nullptr);
    executeSql(
        database,
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE archive ("
        "`index` INTEGER PRIMARY KEY, DirectTo TEXT, FileName TEXT);");
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            database,
            "INSERT INTO archive (`index`, DirectTo, FileName) "
            "VALUES (17, ?, ?)",
            -1, &statement, nullptr),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_bind_text(
            statement, 1, directTo.c_str(), -1, SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_bind_text(
            statement, 2, fileName.c_str(), -1, SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
}

std::pair<std::string, std::string> readAutoPadPath(
    const fs::path& path)
{
    sqlite3* database = nullptr;
    EXPECT_EQ(
        sqlite3_open_v2(
            utf8(path).c_str(), &database,
            SQLITE_OPEN_READONLY, nullptr),
        SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(
        sqlite3_prepare_v2(
            database,
            "SELECT DirectTo, FileName FROM archive WHERE `index`=17",
            -1, &statement, nullptr),
        SQLITE_OK);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto* directTo = sqlite3_column_text(statement, 0);
    const auto* fileName = sqlite3_column_text(statement, 1);
    const std::pair<std::string, std::string> result{
        directTo ? reinterpret_cast<const char*>(directTo) : "",
        fileName ? reinterpret_cast<const char*>(fileName) : ""};
    sqlite3_finalize(statement);
    EXPECT_EQ(sqlite3_close(database), SQLITE_OK);
    return result;
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
TEST(ServiceArchiveNaming, AppendsConfiguredYearOnlyOnce)
{
    EXPECT_EQ(
        searchengine_archive::serviceArchiveDirectoryLeaf(
            L"SearchEngineService", 2026),
        L"SearchEngineService-2026");
    EXPECT_EQ(
        searchengine_archive::serviceArchiveDirectoryLeaf(
            L"SearchEngineService-2026", 2026),
        L"SearchEngineService-2026");
    EXPECT_EQ(
        searchengine_archive::serviceArchiveDirectoryLeaf(
            L"SearchEngineService-2026-prd", 2026),
        L"SearchEngineService-2026-prd-2026");
}

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

TEST(ServiceArchiveCleanup, ResolvesCompletePackagedInstallDirectory)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path installRoot = temporary.path() / L"SearchEngineService-2026";
    const fs::path executable = installRoot / L"bin" / L"SearchEngine.exe";
    writeTextFile(executable, "exe");

    EXPECT_EQ(
        searchengine_archive::serviceInstallDirectory(executable),
        fs::absolute(installRoot).lexically_normal());

    const fs::path legacyExecutable =
        temporary.path() / L"legacy" / L"SearchEngine.exe";
    writeTextFile(legacyExecutable, "exe");
    EXPECT_EQ(
        searchengine_archive::serviceInstallDirectory(legacyExecutable),
        fs::absolute(legacyExecutable.parent_path()).lexically_normal());
}

TEST(ServiceArchiveCleanup, RemovesProgramDataAndCompleteInstallTree)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path installRoot = temporary.path() / L"ProgramFiles" /
        L"SearchEngineService-2026";
    const fs::path dataRoot = temporary.path() / L"ProgramData" /
        L"SearchEngineService-2026";
    writeTextFile(installRoot / L"bin" / L"SearchEngine.exe", "exe");
    writeTextFile(
        installRoot / L"tools" / L"SearchEngineArchive.exe", "tool");
    writeTextFile(installRoot / L"README.txt", "readme");
    writeTextFile(installRoot / L"INSTALLATION_GUIDE_RU.txt", "guide");
    writeTextFile(installRoot / L"ServiceInstance.cmd", "instance");
    writeTextFile(dataRoot / L"Settings.json", "{}");
    writeTextFile(dataRoot / L"inverted_index.sqlite", "index");
    writeTextFile(dataRoot / L"inverted_index.sqlite-wal", "wal");
    writeTextFile(dataRoot / L"inverted_index.sqlite-shm", "shm");

    searchengine_archive::removeServiceRuntimeCleanupDirectories(
        {installRoot, dataRoot});

    EXPECT_FALSE(fs::exists(installRoot));
    EXPECT_FALSE(fs::exists(dataRoot));
}

TEST(ServiceArchiveCleanup, PreflightsEveryRootBeforeDeletingAnything)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path validRoot = temporary.path() / L"valid";
    const fs::path invalidRoot = temporary.path() / L"not-a-directory";
    writeTextFile(validRoot / L"keep.txt", "keep");
    writeTextFile(invalidRoot, "file");

    EXPECT_THROW(
        searchengine_archive::removeServiceRuntimeCleanupDirectories(
            {validRoot, invalidRoot}),
        std::runtime_error);
    EXPECT_TRUE(fs::is_regular_file(validRoot / L"keep.txt"));
    EXPECT_TRUE(fs::is_regular_file(invalidRoot));
}
#endif

TEST(ServiceArchiveCleanup, PreservesDecemberWithArchivedTelegramPaths)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path archived =
        temporary.path() / L"server" / L"autopad" / L"12-2026.db3";
    const fs::path tverdakManager =
        temporary.path() / L"BASES_PRD" / L"METH_BASES" / L"12-2026.db3";
    createAutoPadDatabase(
        archived,
        "E:\\archive\\SearchEngineService-2026\\content\\TLG\\",
        "2026\\226120017.ATL");
    createAutoPadDatabase(
        tverdakManager,
        "D:\\TLG\\",
        "2026\\226120017.ATL");

    ASSERT_NO_THROW(searchengine_archive::replaceRetainedAutoPadDatabase(
        archived, tverdakManager));

    EXPECT_EQ(
        readAutoPadPath(tverdakManager),
        (std::pair<std::string, std::string>{
            "E:\\archive\\SearchEngineService-2026\\content\\TLG\\",
            "2026\\226120017.ATL"}));
    EXPECT_EQ(
        readAutoPadPath(archived),
        readAutoPadPath(tverdakManager));
}

TEST(ServiceArchiveRestore, SilentlyReplacesPreservedDecemberWithActivePaths)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path archived =
        temporary.path() / L"server" / L"autopad" / L"12-2026.db3";
    const fs::path tverdakManager =
        temporary.path() / L"BASES_PRD" / L"METH_BASES" / L"12-2026.db3";
    const std::string archiveRoot =
        "E:\\archive\\SearchEngineService-2026\\content\\TLG\\";
    createAutoPadDatabase(
        archived, archiveRoot, "2026\\226120017.ATL");
    createAutoPadDatabase(
        tverdakManager, archiveRoot, "2026\\old.ATL");
    const std::vector<searchengine_archive::PathMapping> restoreMappings{
        {L"E:\\archive\\SearchEngineService-2026\\content\\TLG",
         L"D:\\TLG"}};

    ASSERT_NO_THROW(searchengine_archive::replaceRetainedAutoPadDatabase(
        archived, tverdakManager, restoreMappings));

    EXPECT_EQ(
        readAutoPadPath(tverdakManager),
        (std::pair<std::string, std::string>{
            "D:\\TLG\\", "2026\\226120017.ATL"}));
    EXPECT_EQ(
        readAutoPadPath(archived),
        (std::pair<std::string, std::string>{
            archiveRoot, "2026\\226120017.ATL"}));
}

TEST(ServiceArchiveRestore, KeepsDecemberUntouchedWhenPathRewriteFails)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path archived =
        temporary.path() / L"server" / L"autopad" / L"12-2026.db3";
    const fs::path tverdakManager =
        temporary.path() / L"BASES_PRD" / L"METH_BASES" / L"12-2026.db3";
    createAutoPadDatabase(
        archived,
        "E:\\archive\\SearchEngineService-2026\\content\\TLG\\",
        "2026\\226120017.ATL");
    createAutoPadDatabase(
        tverdakManager, "D:\\TLG\\", "2026\\original.ATL");
    const std::vector<searchengine_archive::PathMapping> wrongMappings{
        {L"F:\\another-archive\\TLG", L"D:\\TLG"}};

    EXPECT_THROW(
        searchengine_archive::replaceRetainedAutoPadDatabase(
            archived, tverdakManager, wrongMappings),
        std::runtime_error);

    EXPECT_EQ(
        readAutoPadPath(tverdakManager),
        (std::pair<std::string, std::string>{
            "D:\\TLG\\", "2026\\original.ATL"}));
}

TEST(ServiceArchiveCatalog, RewritesOnlyDocumentPathsAndPreservesIdentifiers)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path databasePath = temporary.path() / L"inverted_index.sqlite";
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(utf8(databasePath).c_str(), &database), SQLITE_OK);
    ASSERT_NE(database, nullptr);
    executeSql(
        database,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE docs ("
        "doc_id INTEGER PRIMARY KEY, path TEXT NOT NULL, "
        "mtime_ticks INTEGER NOT NULL, size_int64 INTEGER NOT NULL, "
        "deleted INTEGER NOT NULL DEFAULT 0);"
        "CREATE UNIQUE INDEX idx_docs_path_unique ON docs(path);"
        "CREATE TABLE postings (token TEXT NOT NULL, doc_id INTEGER NOT NULL);"
        "INSERT INTO docs VALUES (7,'D:\\SOURCE\\2026\\file.txt',101,202,0);"
        "INSERT INTO docs VALUES (8,'D:\\SOURCE\\2026\\second.txt',102,203,0);"
        "INSERT INTO docs VALUES (9,'D:\\SOURCE\\TLG\\third.ATL',103,204,0);"
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

TEST(ServiceArchiveCatalog, PreservesFileNamesWhenRootCaseDiffers)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path databasePath = temporary.path() / L"inverted_index.sqlite";
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(utf8(databasePath).c_str(), &database), SQLITE_OK);
    ASSERT_NE(database, nullptr);
    executeSql(
        database,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE docs ("
        "doc_id INTEGER PRIMARY KEY, path TEXT NOT NULL, "
        "mtime_ticks INTEGER NOT NULL, size_int64 INTEGER NOT NULL, "
        "deleted INTEGER NOT NULL DEFAULT 0);"
        "CREATE UNIQUE INDEX idx_docs_path_unique ON docs(path);"
        "CREATE TABLE postings("
        "word_id INTEGER NOT NULL,doc_id INTEGER NOT NULL,cnt INTEGER NOT NULL,"
        "PRIMARY KEY(word_id,doc_id)) WITHOUT ROWID;"
        "INSERT INTO docs VALUES (10,'d:\\source\\tlg\\first.ATL',100,10,0);"
        "INSERT INTO docs VALUES (11,'D:\\Source\\Tlg\\second.SHP',200,20,0);"
        "INSERT INTO postings VALUES (1,10,2);"
        "INSERT INTO postings VALUES (2,11,3);");
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);

    const std::vector<searchengine_archive::PathMapping> mappings{
        {L"D:\\SOURCE\\TLG", L"E:\\ARCHIVE\\content\\TLG"}};
    ASSERT_NO_THROW(searchengine_archive::rewriteDocumentCatalogPaths(
        databasePath, mappings));

    ASSERT_EQ(sqlite3_open_v2(
        utf8(databasePath).c_str(), &database,
        SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        database,
        "SELECT doc_id,path,mtime_ticks,size_int64,deleted FROM docs",
        -1, &statement, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(statement, 0), 10);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        "E:\\ARCHIVE\\content\\TLG\\first.ATL");
    EXPECT_EQ(sqlite3_column_int64(statement, 2), 100);
    EXPECT_EQ(sqlite3_column_int64(statement, 3), 10);
    EXPECT_EQ(sqlite3_column_int(statement, 4), 0);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(statement, 0), 11);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        "E:\\ARCHIVE\\content\\TLG\\second.SHP");
    EXPECT_EQ(sqlite3_column_int64(statement, 2), 200);
    EXPECT_EQ(sqlite3_column_int64(statement, 3), 20);
    EXPECT_EQ(sqlite3_column_int(statement, 4), 0);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);

    ASSERT_EQ(sqlite3_prepare_v2(
        database, "SELECT COUNT(*) FROM postings", -1,
        &statement, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
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
            {"tlg_send_root", "D:\\SOURCE\\2026"},
            {"razn_output_dir", "D:\\SOURCE\\production\\RAZN"},
            {"opis_base_dir", "D:\\SOURCE\\production\\OPIS"},
            {"f12_base_dir", "D:\\SOURCE\\production\\F12"},
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
    EXPECT_EQ(archived.at("tlg_send_root"),
              "E:\\ARCHIVE\\content\\2026");
    EXPECT_EQ(archived.at("razn_output_dir"),
              "E:\\ARCHIVE\\content\\production\\RAZN");
    EXPECT_EQ(archived.at("opis_base_dir"),
              "E:\\ARCHIVE\\content\\production\\OPIS");
    EXPECT_EQ(archived.at("f12_base_dir"),
              "E:\\ARCHIVE\\content\\production\\F12");
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
    EXPECT_EQ(active.at("tlg_send_root"), "D:\\SOURCE\\2026");
    EXPECT_EQ(active.at("razn_output_dir"),
              "D:\\SOURCE\\production\\RAZN");
    EXPECT_EQ(active.at("opis_base_dir"),
              "D:\\SOURCE\\production\\OPIS");
    EXPECT_EQ(active.at("f12_base_dir"),
              "D:\\SOURCE\\production\\F12");
    EXPECT_EQ(active.at("prm_monthly_bases_dir"),
              "D:\\BASES\\METH_BASES");
    EXPECT_EQ(active.at("prd_monthly_bases_dir"),
              "D:\\BASES_PRD\\METH_BASES");
    EXPECT_EQ(active.at("prm_base_dir"), "D:\\BASES");
    EXPECT_EQ(active.at("prd_base_dir"), "D:\\BASES_PRD");
}

TEST(ServiceArchiveRestoreMerge, RemovesStagingAfterSuccessfulTlgMerge)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path staging = temporary.path() / L".TLG.restore-123-1";
    const fs::path target = temporary.path() / L"TLG";
    writeTextFile(staging / L"2026" / L"new.txt", "new");
    writeTextFile(staging / L"COMMON" / L"same.txt", "same");
    writeTextFile(target / L"COMMON" / L"same.txt", "same");

    ASSERT_NO_THROW(
        searchengine_archive::mergeRestoreStagingTree(staging, target));

    EXPECT_FALSE(fs::exists(staging));
    EXPECT_EQ(readTextFile(target / L"2026" / L"new.txt"), "new");
    EXPECT_EQ(readTextFile(target / L"COMMON" / L"same.txt"), "same");
}

TEST(ServiceArchiveRestoreMerge, PreservesStagingWhenTargetFileDiffers)
{
    ServiceArchiveTemporaryDirectory temporary;
    const fs::path staging = temporary.path() / L".TLG.restore-123-1";
    const fs::path target = temporary.path() / L"TLG";
    writeTextFile(staging / L"2026" / L"conflict.txt", "archive");
    writeTextFile(target / L"2026" / L"conflict.txt", "current");

    EXPECT_THROW(
        searchengine_archive::mergeRestoreStagingTree(staging, target),
        std::runtime_error);

    EXPECT_TRUE(fs::exists(staging / L"2026" / L"conflict.txt"));
    EXPECT_EQ(readTextFile(target / L"2026" / L"conflict.txt"), "current");
}

TEST_F(RestoredArchiveDeletionTest, ValidRestoredArchivePassesWithoutScmAccess)
{
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {selected_});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(fs::exists(archiveDirectory_));
}

TEST_F(RestoredArchiveDeletionTest, AcceptsSelectedNonOriginalRestoreRoot)
{
    const fs::path restoredExecutable =
        temporary_.path() / L"selected-restore" / L"program" /
        L"SearchEngine.exe";
    const fs::path restoredData =
        temporary_.path() / L"selected-restore" / L"data";
    writeTextFile(restoredExecutable, "restored-exe");
    writeTextFile(restoredData / L"Settings.json", "{}");
    writeTextFile(restoredData / L"inverted_index.sqlite", "index");
    const std::wstring restoredImage =
        searchengine_archive::buildServiceImagePath(
            restoredExecutable, serviceName_, restoredData);
    manifest_["restore_root"] = utf8(temporary_.path() / L"selected-restore");
    manifest_["restored_image_path"] =
        encoding::wstring_to_utf8(restoredImage);
    manifest_["restored_executable"] = utf8(restoredExecutable);
    manifest_["restored_data_directory"] = utf8(restoredData);
    saveManifest();

    auto restored = selected_;
    restored.imagePath = restoredImage;
    restored.executable = restoredExecutable;
    restored.dataDirectory = restoredData;
    const auto result =
        searchengine_archive::validateRestoredServiceArchiveDeletion(
            archiveDirectory_, {restored});

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
