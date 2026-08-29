#include "ArchiveCore.h"
#include "ServiceArchive.h"
#include "StandBuilder.h"

#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using searchengine_archive_e2e::ServiceArchiveStandOptions;
using searchengine_archive_e2e::StandOptions;
using searchengine_archive_e2e::WorkstationStandOptions;

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            (L"searchengine-archive-e2e-" + std::to_wstring(stamp) + L"-" +
             std::to_wstring(++sequence));
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

fs::path writeSettingsTemplate(const fs::path& directory)
{
    const fs::path path = directory / L"Settings-template.json";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << R"({"config":{"index_roots":["D:\\JANUARY","D:\\FEBRUARY","D:\\MARCH","D:\\APRIL","D:\\MAY","D:\\JUNE","D:\\JULY","D:\\AUGUST","D:\\SEPTEMBER","D:\\OCTOBER","D:\\NOVEMBER","D:\\DECEMBER","D:\\TLG"]}})";
    if (!output)
        throw std::runtime_error("cannot create test Settings template");
    return path;
}

StandOptions standOptions(const TemporaryDirectory& temporary)
{
    StandOptions options;
    options.root = temporary.path() / L"stand-2026";
    options.settingsTemplate = writeSettingsTemplate(temporary.path());
    options.year = 2026;
    options.recordsPerMonth = 10;
    return options;
}

void writeFile(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value;
    ASSERT_TRUE(output.good());
}

json readJsonFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open test JSON");
    json value;
    input >> value;
    return value;
}

int sqliteScalarInt(const fs::path& path, const char* sql)
{
    sqlite3* database = nullptr;
    const std::string utf8Path =
        encoding::wstring_to_utf8(path.wstring());
    if (sqlite3_open_v2(
            utf8Path.c_str(), &database,
            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        const std::string detail = database
            ? sqlite3_errmsg(database) : "cannot allocate SQLite handle";
        sqlite3_close(database);
        throw std::runtime_error("cannot open test SQLite: " + detail);
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW)
    {
        const std::string detail = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("cannot query test SQLite: " + detail);
    }
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

void sqliteExecute(const fs::path& path, const char* sql)
{
    sqlite3* database = nullptr;
    const std::string utf8Path =
        encoding::wstring_to_utf8(path.wstring());
    if (sqlite3_open_v2(
            utf8Path.c_str(), &database,
            SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    {
        const std::string detail = database
            ? sqlite3_errmsg(database) : "cannot allocate SQLite handle";
        sqlite3_close(database);
        throw std::runtime_error("cannot open writable test SQLite: " + detail);
    }
    char* rawError = nullptr;
    const int result = sqlite3_exec(
        database, sql, nullptr, nullptr, &rawError);
    const std::string detail = rawError
        ? rawError : sqlite3_errmsg(database);
    sqlite3_free(rawError);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error("cannot update test SQLite: " + detail);
}

std::vector<fs::path> sqliteDocumentPaths(const fs::path& path)
{
    sqlite3* database = nullptr;
    const std::string utf8Path =
        encoding::wstring_to_utf8(path.wstring());
    if (sqlite3_open_v2(
            utf8Path.c_str(), &database,
            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        const std::string detail = database
            ? sqlite3_errmsg(database) : "cannot allocate SQLite handle";
        sqlite3_close(database);
        throw std::runtime_error("cannot open test SQLite: " + detail);
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database, "SELECT path FROM docs ORDER BY doc_id",
            -1, &statement, nullptr) != SQLITE_OK)
    {
        const std::string detail = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error("cannot read test docs.path: " + detail);
    }
    std::vector<fs::path> result;
    while (true) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW) {
            const std::string detail = sqlite3_errmsg(database);
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("cannot step test docs.path: " + detail);
        }
        const auto* raw = sqlite3_column_text(statement, 0);
        if (!raw) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("test docs.path is NULL");
        }
        result.emplace_back(encoding::utf8_to_wstring(
            reinterpret_cast<const char*>(raw)));
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::string readTextFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open test text file");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

fs::path createRestorePlanArchive(const TemporaryDirectory& temporary)
{
    const fs::path archive = temporary.path() / L"archives" /
        L"SearchEngineService-2026-prd-2026";
    const fs::path program = archive / L"program";
    const fs::path data = archive / L"data";
    const fs::path january = archive / L"content" / L"JANUARY";
    const fs::path tlg = archive / L"content" / L"TLG";
    const fs::path prmDatabase =
        archive / L"autopad" / L"PRM" / L"monthly" / L"01-2026.db3";
    const fs::path prdDatabase =
        archive / L"autopad" / L"PRD" / L"monthly" / L"01-2026.db3";
    const fs::path originalRoot = temporary.path() / L"original-layout";
    const fs::path originalProgram =
        originalRoot / L"ProgramFiles" / L"SearchEngine" / L"bin";
    const fs::path originalData =
        originalRoot / L"ProgramData" / L"SearchEngine";
    const fs::path originalJanuary = originalRoot / L"volume" / L"JANUARY";
    const fs::path originalTlg = originalRoot / L"volume" / L"TLG";
    const fs::path originalPrmMonthly =
        originalRoot / L"volume" / L"BASES" / L"METH_BASES";
    const fs::path originalPrdMonthly =
        originalRoot / L"volume" / L"BASES_PRD" / L"METH_BASES";
    writeFile(program / L"SearchEngine.exe", "exe");
    writeFile(data / L"Settings.json", "{}");
    writeFile(data / L"inverted_index.sqlite", "index");
    writeFile(january / L"incoming.atl", "incoming");
    writeFile(tlg / L"outgoing.atl", "outgoing");
    writeFile(prmDatabase, "prm");
    writeFile(prdDatabase, "prd");

    const std::wstring serviceName = L"SearchEngineService-2026-prd";
    const std::wstring archivedImage =
        searchengine_archive::buildServiceImagePath(
            program / L"SearchEngine.exe", serviceName, data);
    const std::wstring originalImage =
        searchengine_archive::buildServiceImagePath(
            originalProgram / L"SearchEngine.exe",
            serviceName,
            originalData);
    const json manifest{
        {"format_version", 1},
        {"operation", "service-archive"},
        {"phase", "archive-running-source-cleaned"},
        {"year", 2026},
        {"service_name", encoding::wstring_to_utf8(serviceName)},
        {"original_image_path", encoding::wstring_to_utf8(originalImage)},
        {"archived_image_path", encoding::wstring_to_utf8(archivedImage)},
        {"original_executable", encoding::wstring_to_utf8(
            (originalProgram / L"SearchEngine.exe").wstring())},
        {"original_data_directory", encoding::wstring_to_utf8(
            originalData.wstring())},
        {"archived_executable", encoding::wstring_to_utf8(
            (program / L"SearchEngine.exe").wstring())},
        {"archived_data_directory", encoding::wstring_to_utf8(data.wstring())},
        {"original_prm_monthly_directory", encoding::wstring_to_utf8(
            originalPrmMonthly.wstring())},
        {"original_prd_monthly_directory", encoding::wstring_to_utf8(
            originalPrdMonthly.wstring())},
        {"mappings", json::array({
            {{"source", encoding::wstring_to_utf8(originalProgram.wstring())},
             {"target", encoding::wstring_to_utf8(program.wstring())}},
            {{"source", encoding::wstring_to_utf8(originalData.wstring())},
             {"target", encoding::wstring_to_utf8(data.wstring())}},
            {{"source", encoding::wstring_to_utf8(originalJanuary.wstring())},
             {"target", encoding::wstring_to_utf8(january.wstring())}},
            {{"source", encoding::wstring_to_utf8(originalTlg.wstring())},
             {"target", encoding::wstring_to_utf8(tlg.wstring())}}})},
        {"monthly_databases", json::array({
            {{"kind", "PRM"}, {"source", encoding::wstring_to_utf8(
                (originalPrmMonthly / L"01-2026.db3").wstring())},
             {"target", encoding::wstring_to_utf8(prmDatabase.wstring())}},
            {{"kind", "PRD"}, {"source", encoding::wstring_to_utf8(
                (originalPrdMonthly / L"01-2026.db3").wstring())},
             {"target", encoding::wstring_to_utf8(prdDatabase.wstring())}}})}};
    writeFile(archive / L"archive-operation.json", manifest.dump(2));
    return archive;
}

TEST(ArchiveE2EStand, GeneratesAndVerifiesCompleteSyntheticYear)
{
    TemporaryDirectory temporary;
    const StandOptions options = standOptions(temporary);

    const auto generated = searchengine_archive_e2e::generateStand(options);
    EXPECT_EQ(generated.root, fs::absolute(options.root).lexically_normal());
    EXPECT_EQ(generated.year, 2026);
    EXPECT_EQ(generated.databaseCount, 24);
    EXPECT_EQ(generated.telegramRowCount, 240);
    EXPECT_EQ(generated.uniqueTelegramCount, 240);
    EXPECT_EQ(generated.attachmentCount, 240);
    EXPECT_EQ(generated.f12WayRowCount, 240);
    EXPECT_GT(generated.generatedBytes, 0u);

    const auto verified = searchengine_archive_e2e::verifyStand(options.root);
    EXPECT_EQ(verified.databaseCount, 24);
    EXPECT_EQ(verified.telegramRowCount, 240);
    EXPECT_EQ(verified.uniqueTelegramCount, 240);
    EXPECT_EQ(verified.attachmentCount, 240);
    EXPECT_EQ(verified.f12WayRowCount, 240);

    const fs::path f12Database =
        options.root / L"production" / L"F12" / L"2026.db";
    ASSERT_TRUE(fs::is_regular_file(f12Database));
    EXPECT_EQ(sqliteScalarInt(f12Database, "SELECT COUNT(*) FROM way"), 240);
    EXPECT_EQ(
        sqliteScalarInt(f12Database, "SELECT COUNT(*) FROM way WHERE type=1"),
        120);
    EXPECT_EQ(
        sqliteScalarInt(f12Database, "SELECT COUNT(*) FROM way WHERE type=2"),
        120);
    EXPECT_EQ(
        sqliteScalarInt(
            f12Database,
            "SELECT version FROM app_schema_version WHERE database_kind='way'"),
        4);
    EXPECT_EQ(
        sqliteScalarInt(
            f12Database,
            "SELECT COUNT(*) FROM way WHERE source_tab_ind IS NOT NULL "
            "AND operator_name<>''"),
        240);
    EXPECT_EQ(
        sqliteScalarInt(
            f12Database,
            "SELECT COUNT(*) FROM way WHERE number=source_tab_ind"),
        240);

    for (const fs::path& telegram : {
             options.root / L"content" / L"JANUARY" / L"126001001",
             options.root / L"content" / L"TLG" / L"226001001.ATL"})
    {
        const std::string text = readTextFile(telegram);
        EXPECT_NE(text.find(" CODE = "), std::string::npos);
        EXPECT_NE(
            text.find(" DIGITAL = 3141592653 "),
            std::string::npos);
        EXPECT_EQ(text.find("CODE="), std::string::npos);
        EXPECT_EQ(text.find("DIGITAL="), std::string::npos);
    }

    for (int month = 1; month <= 12; ++month) {
        std::wostringstream fileName;
        fileName << std::setfill(L'0') << std::setw(2) << month
                 << L"-2026.db3";
        for (const fs::path& database : {
                 options.root / L"autopad" / L"PRM" / L"METH_BASES" /
                     fileName.str(),
                 options.root / L"autopad" / L"PRD" / L"METH_BASES" /
                     fileName.str()})
        {
            ASSERT_TRUE(fs::is_regular_file(database));
            EXPECT_EQ(
                sqliteScalarInt(
                    database,
                    "SELECT COUNT(*) FROM ARCHIVE WHERE Ekzempl IS NULL "
                    "OR CAST(Ekzempl AS INTEGER)<>1"),
                0);
            EXPECT_EQ(
                sqliteScalarInt(
                    database,
                    "SELECT COUNT(*) FROM ARCHIVE WHERE Lists IS NULL "
                    "OR CAST(Lists AS INTEGER) NOT BETWEEN 1 AND 9"),
                0);
            EXPECT_EQ(
                sqliteScalarInt(
                    database,
                    "SELECT COUNT(DISTINCT CAST(Lists AS INTEGER)) FROM ARCHIVE"),
                9);
        }
    }
    EXPECT_FALSE(fs::exists(options.root / L"autopad" / L"PRM" / L"ARCHIVE.db3"));
    EXPECT_FALSE(fs::exists(options.root / L"autopad" / L"PRD" / L"ARCHIVE.db3"));
    EXPECT_TRUE(fs::is_regular_file(
        options.root / L"server" / L"data" / L"Settings.json"));
    EXPECT_FALSE(fs::exists(options.root / L"content" / L"autopad"));
    EXPECT_TRUE(fs::is_regular_file(
        options.root / L"content" / L"JANUARY" / L"126001001"));
    EXPECT_TRUE(fs::is_regular_file(
        options.root / L"content" / L"TLG" / L"226001001.ATL"));
    EXPECT_TRUE(fs::is_regular_file(
        options.root / L"content" / L"TLG" / L"226001001.zip"));

    for (const auto& item : fs::directory_iterator(temporary.path())) {
        EXPECT_EQ(item.path().filename().wstring().find(L".stand-2026.staging-"),
                  std::wstring::npos);
    }
}

TEST(ArchiveE2EStand, DetectsChangedAttachmentSize)
{
    TemporaryDirectory temporary;
    const StandOptions options = standOptions(temporary);
    (void)searchengine_archive_e2e::generateStand(options);

    const fs::path attachment =
        options.root / L"content" / L"JANUARY" /
        L"PRM_2026_01_1_APP1.bin";
    ASSERT_TRUE(fs::is_regular_file(attachment));
    {
        std::ofstream output(attachment, std::ios::binary | std::ios::trunc);
        output << "changed";
    }
    EXPECT_THROW(
        (void)searchengine_archive_e2e::verifyStand(options.root),
        std::runtime_error);
}

TEST(ArchiveE2EStand, DetectsUnspacedSyntheticTelegramFields)
{
    TemporaryDirectory temporary;
    const StandOptions options = standOptions(temporary);
    (void)searchengine_archive_e2e::generateStand(options);

    const fs::path telegram =
        options.root / L"content" / L"JANUARY" / L"126001001";
    std::string text = readTextFile(telegram);
    const std::size_t position = text.find(" CODE = ");
    ASSERT_NE(position, std::string::npos);
    text.erase(position + 5, 1);
    writeFile(telegram, text);

    EXPECT_THROW(
        (void)searchengine_archive_e2e::verifyStand(options.root),
        std::runtime_error);
}

TEST(ArchiveE2EStand, DetectsF12NumberMissingFromAutoPadIndex)
{
    TemporaryDirectory temporary;
    const StandOptions options = standOptions(temporary);
    (void)searchengine_archive_e2e::generateStand(options);

    const fs::path f12Database =
        options.root / L"production" / L"F12" / L"2026.db";
    sqliteExecute(
        f12Database,
        "UPDATE way SET number=1001 WHERE source_tab_ind=126001001");

    EXPECT_THROW(
        (void)searchengine_archive_e2e::verifyStand(options.root),
        std::runtime_error);
}

TEST(ArchiveE2EStand, GeneratesLegacyRestorableServiceArchiveV3)
{
    TemporaryDirectory temporary;
    ServiceArchiveStandOptions options;
    options.serviceName = L"SearchEngineService-StandV3";
    options.stand = standOptions(temporary);
    options.stand.root = temporary.path() / L"generated" /
        L"SearchEngineService-StandV3-2026";
    options.deploymentRoot = temporary.path() / L"declared-location" /
        L"SearchEngineService-StandV3-2026";
    options.restoreRoot = temporary.path() / L"declared-location" /
        L"restored-stand-v3";
    options.port = 25027;
    options.programTemplate = temporary.path() / L"program-template";
    writeFile(
        options.programTemplate / L"SearchEngine.exe",
        "synthetic executable");
    writeFile(options.programTemplate / L"runtime.dll", "synthetic runtime");
    options.preparerTemplate = temporary.path() / L"preparer-template.exe";
    writeFile(options.preparerTemplate, "synthetic portable preparer");

    const auto generated =
        searchengine_archive_e2e::generateServiceArchiveStand(options);
    EXPECT_EQ(generated.year, 2026);
    EXPECT_EQ(generated.databaseCount, 24);
    EXPECT_EQ(generated.telegramRowCount, 240);
    EXPECT_EQ(generated.f12WayRowCount, 240);
    ASSERT_TRUE(fs::is_regular_file(
        options.stand.root / L"archive-operation.json"));
    ASSERT_TRUE(fs::is_regular_file(
        options.stand.root / L"server" / L"data" /
        L"inverted_index.sqlite"));
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"Activate-Archived-Stand.bat"));
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"README-V3-RU.txt"));
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"Prepare-Archived-Stand.bat"));
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"tools" /
        L"SearchEngineArchiveE2EStand.exe"));

    const std::string activation = readTextFile(
        options.stand.root / L"Activate-Archived-Stand.bat");
    EXPECT_NE(
        activation.find("call \"%STAND_ROOT%\\Prepare-Archived-Stand.bat\""),
        std::string::npos);
    EXPECT_NE(
        activation.find("fsutil.exe dirty query %SystemDrive% >nul 2>&1"),
        std::string::npos);
    EXPECT_EQ(activation.find("^>"), std::string::npos);
    EXPECT_EQ(activation.find("^|"), std::string::npos);
    EXPECT_EQ(
        activation.find(encoding::wstring_to_utf8(
            options.deploymentRoot.wstring())),
        std::string::npos);

    const json archive = readJsonFile(
        options.stand.root / L"archive-operation.json");
    EXPECT_EQ(archive.at("operation"), "service-archive");
    EXPECT_EQ(archive.at("phase"), "archive-running-source-cleaned");
    EXPECT_EQ(archive.at("monthly_databases").size(), 24u);
    EXPECT_EQ(archive.at("mappings").size(), 4u);
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"production" / L"F12" / L"2026.db"));

    const json settings = readJsonFile(
        options.stand.root / L"server" / L"data" / L"Settings.json");
    EXPECT_EQ(settings.at("config").at("asio_port"), 25027);
    EXPECT_EQ(
        settings.at("config").at("razn_output_dir"),
        encoding::wstring_to_utf8(
            (options.restoreRoot / L"production" / L"RAZN").wstring()));

    const auto verified =
        searchengine_archive_e2e::verifyStand(options.stand.root);
    EXPECT_EQ(verified.uniqueTelegramCount, 240);

    const fs::path preparedRestoreRoot =
        options.stand.root.parent_path() / options.restoreRoot.filename();
    const auto prepared =
        searchengine_archive_e2e::prepareServiceArchiveStand(
            options.stand.root);
    EXPECT_EQ(prepared.uniqueTelegramCount, 240);
    const auto preparedAgain =
        searchengine_archive_e2e::prepareServiceArchiveStand(
            options.stand.root);
    EXPECT_EQ(preparedAgain.uniqueTelegramCount, 240);

    const json preparedStand = readJsonFile(
        options.stand.root / L"stand-manifest.json");
    EXPECT_EQ(
        preparedStand.at("root"),
        encoding::wstring_to_utf8(options.stand.root.wstring()));
    EXPECT_EQ(
        preparedStand.at("restore_root"),
        encoding::wstring_to_utf8(preparedRestoreRoot.wstring()));
    const json preparedArchive = readJsonFile(
        options.stand.root / L"archive-operation.json");
    EXPECT_TRUE(preparedArchive.at("portable_prepared").get<bool>());
    EXPECT_EQ(
        preparedArchive.at("archived_data_directory"),
        encoding::wstring_to_utf8(
            (options.stand.root / L"server" / L"data").wstring()));
    EXPECT_EQ(
        preparedArchive.at("original_data_directory"),
        encoding::wstring_to_utf8(
            (preparedRestoreRoot / L"server" / L"data").wstring()));
    const json preparedSettings = readJsonFile(
        options.stand.root / L"server" / L"data" / L"Settings.json");
    EXPECT_EQ(
        preparedSettings.at("config").at("razn_output_dir"),
        encoding::wstring_to_utf8(
            (preparedRestoreRoot / L"production" / L"RAZN").wstring()));

    const auto restorePlan = searchengine_archive::planServiceRestore(
        options.stand.root,
        temporary.path() / L"selected-empty-restore-root");
    EXPECT_EQ(restorePlan.mappings.size(), 4u);
    EXPECT_EQ(restorePlan.monthlyDatabases.size(), 24u);
    EXPECT_EQ(
        restorePlan.restoredExecutable,
        restorePlan.restoreRoot / L"server" / L"program" /
            L"SearchEngine.exe");
}

TEST(ArchiveE2EStand, DeploysWorkstationLayoutUnderDisposableRoots)
{
    TemporaryDirectory temporary;
    ServiceArchiveStandOptions options;
    options.serviceName = L"SearchEngineService-StandV3";
    options.stand = standOptions(temporary);
    options.stand.root = temporary.path() / L"generated" /
        L"SearchEngineService-StandV3-2026";
    options.deploymentRoot = temporary.path() / L"declared-location" /
        L"SearchEngineService-StandV3-2026";
    options.restoreRoot = temporary.path() / L"declared-location" /
        L"restored-stand-v3";
    options.port = 25027;
    options.programTemplate = temporary.path() / L"program-template";
    writeFile(
        options.programTemplate / L"SearchEngine.exe",
        "synthetic executable");
    writeFile(options.programTemplate / L"runtime.dll", "synthetic runtime");
    options.preparerTemplate = temporary.path() / L"preparer-template.exe";
    writeFile(options.preparerTemplate, "synthetic portable preparer");
    options.installerTemplate = temporary.path() / L"portable-package";
    writeFile(
        options.installerTemplate / L"tools" / L"SearchEngineConfig.exe",
        "synthetic config helper");
    writeFile(
        options.installerTemplate / L"tools" / L"SearchEngineArchive.exe",
        "synthetic archive helper");
    writeFile(
        options.installerTemplate / L"prerequisites" /
            L"vc_redist.x86.exe",
        "synthetic redistributable");
    writeFile(
        options.installerTemplate / L"Install-SearchEngineService.bat",
        "@echo off\r\n");

    (void)searchengine_archive_e2e::generateServiceArchiveStand(options);
    ASSERT_TRUE(fs::is_regular_file(
        options.stand.root / L"Deploy-Workstation-Stand.bat"));
    ASSERT_TRUE(fs::is_regular_file(
        options.stand.root / L"README-WORKSTATION-RU.txt"));
    ASSERT_TRUE(fs::is_regular_file(
        options.stand.root / L"installer" /
            L"Install-SearchEngineService.bat"));
    const std::string deployScript = readTextFile(
        options.stand.root / L"Deploy-Workstation-Stand.bat");
    EXPECT_NE(
        deployScript.find("%ProgramFiles(x86)%"),
        std::string::npos);
    EXPECT_NE(
        deployScript.find("deploy-workstation-stand"),
        std::string::npos);
    EXPECT_NE(
        deployScript.find("--data-volume-root \"%DATA_DRIVE%\\.\""),
        std::string::npos);
    EXPECT_EQ(deployScript.find("^>"), std::string::npos);
    EXPECT_EQ(deployScript.find("^|"), std::string::npos);

    const fs::path volume = temporary.path() / L"fake-volume";
    const fs::path programFiles =
        temporary.path() / L"fake-program-files";
    const fs::path programData =
        temporary.path() / L"fake-program-data";
    fs::create_directories(volume);
    fs::create_directories(programFiles);
    fs::create_directories(programData);
    WorkstationStandOptions deployOptions;
    deployOptions.root = options.stand.root;
    deployOptions.dataVolumeRoot = volume;
    deployOptions.programFilesRoot = programFiles;
    deployOptions.programDataRoot = programData;

    const fs::path conflictVolume =
        temporary.path() / L"conflict-volume";
    const fs::path conflictProgramFiles =
        temporary.path() / L"conflict-program-files";
    const fs::path conflictProgramData =
        temporary.path() / L"conflict-program-data";
    fs::create_directories(
        conflictVolume / L"BASES" / L"METH_BASES");
    fs::create_directories(conflictProgramFiles);
    fs::create_directories(conflictProgramData);
    writeFile(
        conflictVolume / L"BASES" / L"METH_BASES" / L"keep.txt",
        "keep");
    WorkstationStandOptions conflictOptions = deployOptions;
    conflictOptions.dataVolumeRoot = conflictVolume;
    conflictOptions.programFilesRoot = conflictProgramFiles;
    conflictOptions.programDataRoot = conflictProgramData;
    try {
        (void)searchengine_archive_e2e::deployWorkstationStand(
            conflictOptions);
        FAIL() << "non-empty METH_BASES unexpectedly passed preflight";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("METH_BASES"), std::string::npos) << message;
        EXPECT_NE(message.find("not empty"), std::string::npos) << message;
    }
    EXPECT_FALSE(fs::exists(
        conflictProgramFiles / options.serviceName));
    EXPECT_FALSE(fs::exists(
        conflictProgramData / options.serviceName));
    EXPECT_FALSE(fs::exists(conflictVolume / L"TLG"));

    const json undeployedStand = readJsonFile(
        options.stand.root / L"stand-manifest.json");
    fs::create_directories(programFiles / options.serviceName);
    fs::create_directories(programData / options.serviceName);
    writeFile(volume / L"BASES" / L"root-sentinel.txt", "keep");
    writeFile(volume / L"BASES_PRD" / L"root-sentinel.txt", "keep");
    fs::create_directories(volume / L"BASES" / L"METH_BASES");
    fs::create_directories(volume / L"BASES_PRD" / L"METH_BASES");
    fs::create_directories(volume / L"TLG");
    fs::create_directories(volume / L"OPIS_ADMIN");
    fs::create_directories(volume / L"F12");
    for (const auto& name :
         undeployedStand.at("content_layout").at("prm_month_directories"))
    {
        fs::create_directories(
            volume /
            encoding::utf8_to_wstring(name.get<std::string>()));
    }

    const auto layout =
        searchengine_archive_e2e::deployWorkstationStand(deployOptions);
    EXPECT_EQ(layout.dataVolumeRoot, fs::absolute(volume).lexically_normal());
    EXPECT_TRUE(fs::is_regular_file(
        layout.binDirectory / L"SearchEngine.exe"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.toolsDirectory / L"SearchEngineConfig.exe"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.dataDirectory / L"Settings.json"));
    EXPECT_TRUE(fs::is_directory(layout.dataDirectory / L"logs"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.monthDirectories.at(0) / L"126001001"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.tlgDirectory / L"226001001.ATL"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.prmMonthlyDirectory / L"01-2026.db3"));
    EXPECT_TRUE(fs::is_regular_file(
        layout.prdMonthlyDirectory / L"12-2026.db3"));
    EXPECT_EQ(
        std::ifstream(volume / L"BASES" / L"root-sentinel.txt").peek(),
        static_cast<int>('k'));
    EXPECT_EQ(
        std::ifstream(volume / L"BASES_PRD" / L"root-sentinel.txt").peek(),
        static_cast<int>('k'));
    EXPECT_TRUE(fs::is_directory(layout.opisDirectory));
    EXPECT_TRUE(fs::is_directory(layout.raznDirectory));
    EXPECT_TRUE(fs::is_directory(layout.f12Directory));
    const fs::path deployedF12 = layout.f12Directory / L"2026.db";
    ASSERT_TRUE(fs::is_regular_file(deployedF12));
    EXPECT_EQ(sqliteScalarInt(deployedF12, "SELECT COUNT(*) FROM way"), 240);
    EXPECT_EQ(
        sqliteScalarInt(deployedF12, "SELECT COUNT(*) FROM way WHERE type=1"),
        120);
    EXPECT_EQ(
        sqliteScalarInt(deployedF12, "SELECT COUNT(*) FROM way WHERE type=2"),
        120);
    EXPECT_EQ(
        sqliteScalarInt(
            deployedF12,
            "SELECT COUNT(*) FROM way WHERE number=source_tab_ind"),
        240);
    EXPECT_TRUE(fs::is_regular_file(
        options.stand.root / L"workstation-deployment.json"));

    const json deployment = readJsonFile(
        options.stand.root / L"workstation-deployment.json");
    EXPECT_EQ(
        deployment.at("f12_database"),
        encoding::wstring_to_utf8(deployedF12.wstring()));
    EXPECT_EQ(deployment.at("f12_way_rows"), 240);

    const json settings =
        readJsonFile(layout.dataDirectory / L"Settings.json");
    const json& config = settings.at("config");
    EXPECT_EQ(config.at("index_roots").size(), 13u);
    EXPECT_EQ(
        config.at("index_roots").at(0),
        encoding::wstring_to_utf8(
            layout.monthDirectories.at(0).wstring()));
    EXPECT_EQ(
        config.at("index_roots").at(12),
        encoding::wstring_to_utf8(layout.tlgDirectory.wstring()));
    EXPECT_EQ(
        config.at("prm_base_dir"),
        encoding::wstring_to_utf8(layout.prmBaseDirectory.wstring()));
    EXPECT_EQ(
        config.at("prd_base_dir"),
        encoding::wstring_to_utf8(layout.prdBaseDirectory.wstring()));
    EXPECT_EQ(
        config.at("tlg_send_root"),
        encoding::wstring_to_utf8(layout.dataVolumeRoot.wstring()));
    EXPECT_EQ(
        config.at("razn_output_dir"),
        encoding::wstring_to_utf8(layout.raznDirectory.wstring()));
    EXPECT_EQ(config.at("server_mode"), "active");
    EXPECT_EQ(config.at("document_catalog_storage"), "sqlite");

    const auto prmRoots = searchengine_archive::inspectAutoPadDirectToRoots(
        layout.prmMonthlyDirectory / L"01-2026.db3");
    ASSERT_EQ(prmRoots.size(), 1u);
    EXPECT_EQ(prmRoots.front(), layout.monthDirectories.at(0));
    const auto prdRoots = searchengine_archive::inspectAutoPadDirectToRoots(
        layout.prdMonthlyDirectory / L"01-2026.db3");
    ASSERT_EQ(prdRoots.size(), 1u);
    EXPECT_EQ(prdRoots.front(), layout.tlgDirectory);

    const fs::path deployedIndex =
        layout.dataDirectory / L"inverted_index.sqlite";
    sqliteExecute(
        deployedIndex,
        "PRAGMA journal_mode=WAL;"
        "UPDATE meta SET value=value WHERE key='schema_version';");
    const fs::path frozenContent =
        temporary.path() / L"frozen-archive" / L"content";
    std::vector<searchengine_archive::PathMapping> freezeMappings;
    for (const auto& month : layout.monthDirectories) {
        freezeMappings.push_back({
            month,
            frozenContent / month.filename()});
    }
    freezeMappings.push_back({
        layout.tlgDirectory,
        frozenContent / layout.tlgDirectory.filename()});
    ASSERT_NO_THROW(searchengine_archive::rewriteDocumentCatalogPaths(
        deployedIndex, freezeMappings));
    const auto frozenDocumentPaths = sqliteDocumentPaths(deployedIndex);
    ASSERT_EQ(frozenDocumentPaths.size(), 240u);
    for (const auto& document : frozenDocumentPaths) {
        EXPECT_TRUE(searchengine_archive::isPathEqualOrBelow(
            document, frozenContent)) << document.string();
    }
    std::string indexIntegrityError;
    EXPECT_TRUE(verifySQLiteDatabase(
        deployedIndex, indexIntegrityError)) << indexIntegrityError;

    writeFile(layout.tlgDirectory / L"keep.txt", "keep");
    EXPECT_THROW(
        (void)searchengine_archive_e2e::planWorkstationStandDeployment(
            deployOptions),
        std::runtime_error);
    EXPECT_EQ(
        std::ifstream(layout.tlgDirectory / L"keep.txt").peek(),
        static_cast<int>('k'));
}

TEST(ArchiveE2EStand, RunsCompleteYearMoveAndManifestProvenCleanup)
{
    TemporaryDirectory temporary;
    const StandOptions options = standOptions(temporary);
    (void)searchengine_archive_e2e::generateStand(options);

    searchengine_archive::YearMoveOptions moveOptions;
    moveOptions.year = options.year;
    moveOptions.prmMonthlyDirectory =
        options.root / L"autopad" / L"PRM" / L"METH_BASES";
    moveOptions.prdMonthlyDirectory =
        options.root / L"autopad" / L"PRD" / L"METH_BASES";
    moveOptions.archiveRoot = temporary.path() / L"moved-year";

    const auto plan = searchengine_archive::planYearMove(moveOptions);
    ASSERT_EQ(plan.databases.size(), 24u);
    ASSERT_EQ(plan.mappings.size(), 13u);
    ASSERT_EQ(plan.warnings.size(), 2u);
    for (const auto& warning : plan.warnings)
        EXPECT_NE(warning.find("13"), std::string::npos);

    const auto moved = searchengine_archive::executeYearMove(plan);
    ASSERT_TRUE(moved.ok) << moved.message;
    EXPECT_TRUE(fs::is_regular_file(moved.manifestPath));
    EXPECT_EQ(moved.copiedFiles, 444u);
    EXPECT_GT(moved.copiedBytes, 0u);

    for (const auto& database : plan.databases) {
        const fs::path archived = moved.finalDirectory / database.relativeTarget;
        ASSERT_TRUE(fs::is_regular_file(archived));
        const auto roots = searchengine_archive::inspectAutoPadDirectToRoots(archived);
        ASSERT_EQ(roots.size(), 1u);
        EXPECT_TRUE(searchengine_archive::isPathEqualOrBelow(
            roots.front(), moved.finalDirectory));
    }
    EXPECT_FALSE(fs::exists(
        moved.finalDirectory / L"autopad" / L"PRM" / L"ARCHIVE.db3"));
    EXPECT_FALSE(fs::exists(
        moved.finalDirectory / L"autopad" / L"PRD" / L"ARCHIVE.db3"));

    const auto cleaned = searchengine_archive::cleanupYearMoveFiles(
        moved.finalDirectory, true);
    ASSERT_TRUE(cleaned.ok) << cleaned.message;
    for (const auto& mapping : plan.mappings)
        EXPECT_FALSE(fs::exists(mapping.source));
    EXPECT_FALSE(fs::exists(
        moveOptions.prmMonthlyDirectory / L"01-2026.db3"));
    EXPECT_FALSE(fs::exists(
        moveOptions.prdMonthlyDirectory / L"11-2026.db3"));
    EXPECT_TRUE(fs::is_regular_file(
        moveOptions.prdMonthlyDirectory / L"12-2026.db3"));
    EXPECT_TRUE(fs::is_directory(moved.finalDirectory));
}

TEST(ServiceRestorePlan, RebasesArchiveLayoutUnderSelectedRoot)
{
    TemporaryDirectory temporary;
    const fs::path archive = createRestorePlanArchive(temporary);
    const fs::path restoreRoot = temporary.path() / L"selected-empty-root";

    const auto plan = searchengine_archive::planServiceRestore(
        archive, restoreRoot);

    EXPECT_EQ(plan.restoreRoot, fs::absolute(restoreRoot).lexically_normal());
    EXPECT_EQ(plan.restoredExecutable,
              plan.restoreRoot / L"program" / L"SearchEngine.exe");
    EXPECT_EQ(plan.restoredDataDirectory, plan.restoreRoot / L"data");
    EXPECT_EQ(plan.restoredPrmMonthlyDirectory,
              plan.restoreRoot / L"autopad" / L"PRM" / L"monthly");
    EXPECT_EQ(plan.restoredPrdMonthlyDirectory,
              plan.restoreRoot / L"autopad" / L"PRD" / L"monthly");
    ASSERT_EQ(plan.mappings.size(), 4u);
    EXPECT_EQ(plan.mappings.at(2).target,
              plan.restoreRoot / L"content" / L"JANUARY");
    EXPECT_EQ(plan.mappings.at(3).target,
              plan.restoreRoot / L"content" / L"TLG");
    EXPECT_FALSE(fs::exists(restoreRoot));
}

TEST(ServiceRestorePlan, NormalizesBareDriveAndRejectsOtherRelativeRoots)
{
    EXPECT_EQ(
        searchengine_archive::normalizeServiceRestoreRoot(L"D:"),
        fs::path(L"D:\\"));
    EXPECT_EQ(
        searchengine_archive::normalizeServiceRestoreRoot(L"D:\\"),
        fs::path(L"D:\\"));
    EXPECT_THROW(
        (void)searchengine_archive::normalizeServiceRestoreRoot(
            L"relative-folder"),
        std::runtime_error);
    EXPECT_THROW(
        (void)searchengine_archive::normalizeServiceRestoreRoot(
            L"D:relative-folder"),
        std::runtime_error);
}

TEST(ServiceRestorePlan, RestoresRecordedOriginalLocationsWithoutArchiveFolders)
{
    TemporaryDirectory temporary;
    const fs::path archive = createRestorePlanArchive(temporary);

    const auto plan =
        searchengine_archive::planServiceRestoreOriginalLocations(archive);

    EXPECT_EQ(
        plan.mode,
        searchengine_archive::ServiceRestoreMode::OriginalLocations);
    EXPECT_TRUE(plan.restoreRoot.empty());
    EXPECT_TRUE(plan.sourceCleanupCompleted);
    EXPECT_EQ(
        plan.restoredExecutable,
        temporary.path() / L"original-layout" / L"ProgramFiles" /
            L"SearchEngine" / L"bin" / L"SearchEngine.exe");
    EXPECT_EQ(
        plan.restoredDataDirectory,
        temporary.path() / L"original-layout" / L"ProgramData" /
            L"SearchEngine");
    ASSERT_EQ(plan.mappings.size(), 4u);
    EXPECT_EQ(
        plan.mappings.at(2).target,
        temporary.path() / L"original-layout" / L"volume" / L"JANUARY");
    EXPECT_EQ(
        plan.mappings.at(3).target,
        temporary.path() / L"original-layout" / L"volume" / L"TLG");
    ASSERT_EQ(plan.monthlyDatabases.size(), 2u);
    EXPECT_EQ(
        plan.monthlyDatabases.at(0).target,
        temporary.path() / L"original-layout" / L"volume" / L"BASES" /
            L"METH_BASES" / L"01-2026.db3");
    EXPECT_FALSE(fs::exists(temporary.path() / L"original-layout"));
}

TEST(ServiceRestorePlan, RejectsAnyDestinationDirectoryBeforeCopying)
{
    TemporaryDirectory temporary;
    const fs::path archive = createRestorePlanArchive(temporary);
    const fs::path restoreRoot = temporary.path() / L"selected-root";
    writeFile(restoreRoot / L"content" / L"TLG" / L"existing.txt", "keep");

    try {
        (void)searchengine_archive::planServiceRestore(archive, restoreRoot);
        FAIL() << "collision preflight unexpectedly succeeded";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("no merge or overwrite"), std::string::npos);
        EXPECT_NE(message.find("Choose another restore root"),
                  std::string::npos);
    }

    EXPECT_EQ(
        std::ifstream(restoreRoot / L"content" / L"TLG" / L"existing.txt")
            .peek(),
        static_cast<int>('k'));
    EXPECT_FALSE(fs::exists(restoreRoot / L"program"));
    EXPECT_FALSE(fs::exists(restoreRoot / L"data"));
    EXPECT_FALSE(fs::exists(restoreRoot / L"autopad"));
}

} // namespace
