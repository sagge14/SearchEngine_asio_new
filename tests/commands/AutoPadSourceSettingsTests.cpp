#include "Commands/CommandResult.h"
#include "Commands/GetIshTelegaPdtv/GetIshTelegaPdtvCommand.h"
#include "Commands/GetJsonTelega/AutoPadSource.h"
#include "Commands/GetJsonTelega/GetJsonTelegaCmd.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "JSON/ConverterJSON.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/sqlite3.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;
    using command_execution::ErrorCode;

    std::vector<std::uint8_t> bytesOf(const std::string& text)
    {
        return {text.begin(), text.end()};
    }

    void executeSql(sqlite3* database, const char* sql)
    {
        char* error = nullptr;
        const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        ASSERT_EQ(result, SQLITE_OK) << message;
    }

    void createMinimalArchive(const fs::path& databasePath, bool withRow)
    {
        sqlite3* database = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &database), SQLITE_OK);
        ASSERT_NE(database, nullptr);

        executeSql(
            database,
            "CREATE TABLE ARCHIVE ("
            "`index` TEXT, DData TEXT, FFrom TEXT, FFrom1 TEXT, TelNo TEXT, "
            "PodpNo TEXT, DataPodp TEXT, Familia TEXT, Copyes TEXT, "
            "PrilName1 TEXT, FFrom5 TEXT, PrilName TEXT, KolPril TEXT, "
            "DirectTo TEXT, FileName TEXT, Copyes2 TEXT, Blank TEXT, "
            "Edit TEXT, GdeSHT TEXT)");

        if (withRow) {
            executeSql(
                database,
                "INSERT INTO ARCHIVE VALUES ("
                "'1','20240101','from','from1','100','p1','20240102',"
                "'isp','copy','kr','kr2','pril','0','C:\\\\tmp\\\\','1',"
                "'blank','blank2','edit','gde')");
        }

        ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    }

    void closeCachedConnections(const fs::path& databasePath)
    {
        SQLiteConnectionManager::instance().closeConnection(databasePath.string());
        Telega::b_prm.clear();
        Telega::b_prd.clear();
    }

    bool sentinelExists(const fs::path& databasePath)
    {
        sqlite3* database = nullptr;
        if (sqlite3_open(databasePath.string().c_str(), &database) != SQLITE_OK)
            return false;

        sqlite3_stmt* statement = nullptr;
        const int prepare = sqlite3_prepare_v2(
            database,
            "SELECT COUNT(*) FROM sentinel WHERE id = 42",
            -1,
            &statement,
            nullptr);
        int count = 0;
        if (prepare == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
            count = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return count == 1;
    }

    void createPdtvArchive(
        const fs::path& databasePath,
        const std::string& pdtv,
        const std::string& allPdtv1)
    {
        sqlite3* database = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &database), SQLITE_OK);
        ASSERT_NE(database, nullptr);
        executeSql(database, "DROP TABLE IF EXISTS ARCHIVE");
        executeSql(
            database,
            "CREATE TABLE ARCHIVE ("
            "`index` TEXT, pdtv TEXT, allpdtv1 TEXT)");
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(
            sqlite3_prepare_v2(
                database,
                "INSERT INTO ARCHIVE (`index`, pdtv, allpdtv1) VALUES (?, ?, ?)",
                -1,
                &statement,
                nullptr),
            SQLITE_OK);
        ASSERT_EQ(sqlite3_bind_text(statement, 1, "7", -1, SQLITE_TRANSIENT), SQLITE_OK);
        ASSERT_EQ(
            sqlite3_bind_text(statement, 2, pdtv.c_str(), -1, SQLITE_TRANSIENT),
            SQLITE_OK);
        ASSERT_EQ(
            sqlite3_bind_text(
                statement,
                3,
                allPdtv1.c_str(),
                -1,
                SQLITE_TRANSIENT),
            SQLITE_OK);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
        ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
        executeSql(database, "CREATE TABLE sentinel (id INTEGER)");
        executeSql(database, "INSERT INTO sentinel VALUES (42)");
        ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    }

    class AutoPadSourceTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            static std::atomic_uint64_t sequence{0};
            const auto uniqueValue =
                std::chrono::steady_clock::now().time_since_epoch().count() +
                static_cast<std::int64_t>(
                    sequence.fetch_add(1, std::memory_order_relaxed));
            root_ = fs::temp_directory_path() /
                ("searchengine-autopad-source-" + std::to_string(uniqueValue));
            prmDir_ = root_ / "BASES";
            prdDir_ = root_ / "BASES_PRD";
            fs::create_directories(prmDir_);
            fs::create_directories(prdDir_);

            prmArchive_ = prmDir_ / "ARCHIVE.db3";
            prdArchive_ = prdDir_ / "ARCHIVE.db3";
            createMinimalArchive(prmArchive_, true);
            createMinimalArchive(prdArchive_, true);

            Telega::year = "2099";
            Telega::prm_base_dir = prmDir_.string();
            Telega::prd_base_dir = prdDir_.string();
            Telega::prm_monthly_bases_dir.clear();
            Telega::prd_monthly_bases_dir.clear();
            Telega::archive_mode = false;
            Telega::b_prm.clear();
            Telega::b_prd.clear();
        }

        void TearDown() override
        {
            Telega::b_prm.clear();
            Telega::b_prd.clear();
            Telega::prm_base_dir.clear();
            Telega::prd_base_dir.clear();
            Telega::prm_monthly_bases_dir.clear();
            Telega::prd_monthly_bases_dir.clear();
            Telega::archive_mode = false;
            SQLiteConnectionManager::instance().closeConnection(prmArchive_.string());
            SQLiteConnectionManager::instance().closeConnection(prdArchive_.string());

            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }

        fs::path root_;
        fs::path prmDir_;
        fs::path prdDir_;
        fs::path prmArchive_;
        fs::path prdArchive_;
    };

    // Mirrors ConverterJSON acceptance: field must be a string; empty is valid.
    bool acceptBaseDirSetting(
        const nh::json& config,
        const char* key,
        std::string& out,
        std::vector<std::string>& errors)
    {
        if (!config.contains(key) || !config[key].is_string()) {
            errors.emplace_back(
                std::string("config.") + key +
                " (must be a string; empty disables source)");
            return false;
        }
        out = config[key].get<std::string>();
        return true;
    }
}

TEST(AutoPadSettings, BothBaseDirsEmptyAreValid)
{
    const nh::json config = {
        {"prm_base_dir", ""},
        {"prd_base_dir", ""},
        {"year", "2026"},
    };
    std::string prm;
    std::string prd;
    std::vector<std::string> errors;
    EXPECT_TRUE(acceptBaseDirSetting(config, "prm_base_dir", prm, errors));
    EXPECT_TRUE(acceptBaseDirSetting(config, "prd_base_dir", prd, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(prm.empty());
    EXPECT_TRUE(prd.empty());
}

TEST(AutoPadSettings, OneBaseDirEmptyIsValid)
{
    const nh::json config = {
        {"prm_base_dir", ""},
        {"prd_base_dir", "D:\\BASES_PRD"},
    };
    std::string prm;
    std::string prd;
    std::vector<std::string> errors;
    EXPECT_TRUE(acceptBaseDirSetting(config, "prm_base_dir", prm, errors));
    EXPECT_TRUE(acceptBaseDirSetting(config, "prd_base_dir", prd, errors));
    EXPECT_TRUE(prm.empty());
    EXPECT_EQ(prd, "D:\\BASES_PRD");
}

TEST(AutoPadTypeDetection, ExtensionMapsToSourceType)
{
    EXPECT_EQ(
        Telega::getTypeFromDir(fs::path("D:/files/12345")),
        Telega::TYPE::VHOD);
    EXPECT_EQ(
        Telega::getTypeFromDir(fs::path("D:/files/12345.tlg")),
        Telega::TYPE::ISHOD);
}

TEST_F(AutoPadSourceTest, GetBasesDisabledDoesNotTouchFilesystem)
{
    Telega::prm_base_dir.clear();
    const auto bases = Telega::getBases(Telega::TYPE::VHOD);
    EXPECT_TRUE(bases.empty());
    EXPECT_FALSE(Telega::isSourceConfigured(Telega::TYPE::VHOD));
    EXPECT_EQ(
        Telega::probeSource(Telega::TYPE::VHOD),
        Telega::SourceAvailability::Disabled);
}

TEST_F(AutoPadSourceTest, ArchiveModeUsesOnlyExplicitMonthlyDirectory)
{
    const fs::path monthly = root_ / "archive-prm-monthly";
    fs::create_directories(monthly);
    createMinimalArchive(monthly / "12-2099.db3", true);
    createMinimalArchive(monthly / "01-2100.db3", true);

    Telega::archive_mode = true;
    Telega::prm_monthly_bases_dir = monthly.string();
    Telega::prm_base_dir = prmDir_.string();

    const auto bases = Telega::getBases(Telega::TYPE::VHOD);
    ASSERT_EQ(bases.size(), 1u);
    EXPECT_EQ(fs::path(bases.front()), monthly / "12-2099.db3");
    EXPECT_EQ(
        Telega::probeSource(Telega::TYPE::VHOD),
        Telega::SourceAvailability::Configured);
    EXPECT_TRUE(Telega::archiveDbPathFor(Telega::TYPE::VHOD).empty());
}

TEST_F(AutoPadSourceTest, MissingOperationalArchiveIsNeverCreatedBySearch)
{
    SQLiteConnectionManager::instance().closeConnection(prmArchive_.string());
    SQLiteConnectionManager::instance().closeConnection(prdArchive_.string());
    ASSERT_TRUE(fs::remove(prmArchive_));
    ASSERT_TRUE(fs::remove(prdArchive_));

    GetJsonTelegaVhCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 1"));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_FALSE(fs::exists(prmArchive_));
    EXPECT_FALSE(fs::exists(prdArchive_));
    EXPECT_TRUE(Telega::b_prm.empty());
}

TEST_F(AutoPadSourceTest, SqlVhDisabledReturnsEmptySuccess)
{
    Telega::prm_base_dir.clear();
    GetJsonTelegaVhCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 1"));
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    EXPECT_TRUE(payload.empty());
    EXPECT_TRUE(Telega::b_prm.empty());
}

TEST_F(AutoPadSourceTest, SqlIshDisabledReturnsEmptySuccess)
{
    Telega::prd_base_dir.clear();
    GetJsonTelegaIshCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 1"));
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    EXPECT_TRUE(payload.empty());
    EXPECT_TRUE(Telega::b_prd.empty());
}

TEST_F(AutoPadSourceTest, SqlBothDisabledReturnEmptyWithoutTypedError)
{
    Telega::prm_base_dir.clear();
    Telega::prd_base_dir.clear();

    GetJsonTelegaVhCmd vh;
    GetJsonTelegaIshCmd ish;
    const auto vhResult = vh.executeResult(bytesOf("`index` = 1"));
    const auto ishResult = ish.executeResult(bytesOf("`index` = 1"));

    ASSERT_TRUE(vhResult.succeeded()) << vhResult.diagnostic;
    ASSERT_TRUE(ishResult.succeeded()) << ishResult.diagnostic;
    EXPECT_FALSE(vhResult.error.has_value());
    EXPECT_FALSE(ishResult.error.has_value());

    const auto vhPayload = nh::json::parse(
        std::string(vhResult.payload.begin(), vhResult.payload.end()));
    const auto ishPayload = nh::json::parse(
        std::string(ishResult.payload.begin(), ishResult.payload.end()));
    EXPECT_TRUE(vhPayload.is_array());
    EXPECT_TRUE(vhPayload.empty());
    EXPECT_TRUE(ishPayload.is_array());
    EXPECT_TRUE(ishPayload.empty());
}

TEST_F(AutoPadSourceTest, SqlVhMissingDirReturnsDataSourceUnavailable)
{
    Telega::prm_base_dir = (root_ / "missing-prm").string();
    GetJsonTelegaVhCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 1"));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
}

TEST_F(AutoPadSourceTest, SqlIshMissingDirReturnsDataSourceUnavailable)
{
    Telega::prd_base_dir = (root_ / "missing-prd").string();
    GetJsonTelegaIshCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 1"));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
}

TEST_F(AutoPadSourceTest, SqlEmptyResultIsNormalEmptyArray)
{
    Telega::year = "2099";
    GetJsonTelegaVhCmd command;
    const auto result = command.executeResult(bytesOf("`index` = 999999"));
    ASSERT_TRUE(result.succeeded());
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    EXPECT_TRUE(payload.empty());
}

TEST_F(AutoPadSourceTest, TextSearchSkipsDisabledPrmKeepsPrd)
{
    Telega::prm_base_dir.clear();
    listAnswer hits;
    hits.push_back(AnswerItem{"D:/x/111", 1.0f, false});
    hits.push_back(AnswerItem{"D:/x/222.tlg", 1.0f, false});
    hits.push_back(AnswerItem{"D:/x/333", 0.5f, false});

    const auto result = buildTelegiJsonFromSearchHits(hits);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload[0]["type"].get<int>(), static_cast<int>(Telega::TYPE::ISHOD));
    EXPECT_TRUE(Telega::b_prm.empty());
}

TEST_F(AutoPadSourceTest, TextSearchSkipsDisabledPrdKeepsPrm)
{
    Telega::prd_base_dir.clear();
    listAnswer hits;
    hits.push_back(AnswerItem{"D:/x/111", 1.0f, false});
    hits.push_back(AnswerItem{"D:/x/222.tlg", 1.0f, false});

    const auto result = buildTelegiJsonFromSearchHits(hits);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload[0]["type"].get<int>(), static_cast<int>(Telega::TYPE::VHOD));
}

TEST_F(AutoPadSourceTest, TextSearchBothDisabledReturnsEmptyWithoutError)
{
    Telega::prm_base_dir.clear();
    Telega::prd_base_dir.clear();
    listAnswer hits;
    hits.push_back(AnswerItem{"D:/x/111", 1.0f, false});
    hits.push_back(AnswerItem{"D:/x/222.tlg", 1.0f, false});

    const auto result = buildTelegiJsonFromSearchHits(hits);
    ASSERT_TRUE(result.succeeded());
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    EXPECT_TRUE(payload.empty());
}

TEST_F(AutoPadSourceTest, TextSearchOnlyPrdHitsDoesNotTouchBrokenPrm)
{
    Telega::prm_base_dir = (root_ / "broken-prm").string();
    listAnswer hits;
    hits.push_back(AnswerItem{"D:/x/222.tlg", 1.0f, false});

    const auto result = buildTelegiJsonFromSearchHits(hits);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_TRUE(Telega::b_prm.empty());
}

TEST_F(AutoPadSourceTest, TextSearchPrmHitWithBrokenPrmFailsUnavailable)
{
    Telega::prm_base_dir = (root_ / "broken-prm").string();
    listAnswer hits;
    hits.push_back(AnswerItem{"D:/x/111", 1.0f, false});
    hits.push_back(AnswerItem{"D:/x/222.tlg", 1.0f, false});

    const auto result = buildTelegiJsonFromSearchHits(hits);
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
}

TEST_F(AutoPadSourceTest, HardcodedBasesNotUsedWhenPrmDisabled)
{
    Telega::prm_base_dir.clear();
    Telega::prd_base_dir = prdDir_.string();
    EXPECT_TRUE(Telega::archiveDbPathFor(Telega::TYPE::VHOD).empty());
    EXPECT_EQ(
        Telega::archiveDbPathFor(Telega::TYPE::VHOD),
        std::string{});
    EXPECT_NE(
        Telega::archiveDbPathFor(Telega::TYPE::ISHOD),
        "D:\\BASES_PRD\\ARCHIVE.DB3");
    EXPECT_EQ(
        Telega::archiveDbPathFor(Telega::TYPE::ISHOD),
        prdDir_.string() + "\\ARCHIVE.DB3");
}

TEST_F(AutoPadSourceTest, PdtvDisabledReturnsEmptySuccess)
{
    Telega::prd_base_dir.clear();
    GetIshTelegaPdtvCommand command;
    const auto result = command.executeResult(bytesOf("1"));
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    EXPECT_TRUE(payload.empty());
    EXPECT_TRUE(Telega::b_prd.empty());
}

TEST_F(AutoPadSourceTest, PdtvMissingSourceReturnsDataSourceUnavailable)
{
    Telega::prd_base_dir = (root_ / "missing-prd").string();
    GetIshTelegaPdtvCommand command;
    const auto result = command.executeResult(bytesOf("1"));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
}

TEST_F(AutoPadSourceTest, PdtvMissingSchemaReturnsDatabaseSchemaFailed)
{
    GetIshTelegaPdtvCommand command;
    const auto result = command.executeResult(bytesOf("1"));
    ASSERT_TRUE(result.failed()) << result.diagnostic;
    EXPECT_EQ(result.error, ErrorCode::DatabaseSchemaFailed);
}

TEST_F(AutoPadSourceTest, PdtvValidRowReturnsMetadata)
{
    closeCachedConnections(prdArchive_);
    createPdtvArchive(
        prdArchive_,
        "confirm.txt|20240101|C:\\tmp\\confirm.txt",
        "a.txt|20240101|C:\\tmp\\a.txt/b.txt|20240102|C:\\tmp\\b.txt");

    GetIshTelegaPdtvCommand command;
    const auto result = command.executeResult(bytesOf("7"));
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    const auto payload = nh::json::parse(
        std::string(result.payload.begin(), result.payload.end()));
    ASSERT_TRUE(payload.is_array());
    ASSERT_EQ(payload.size(), 1u);
    const auto& row = payload.front();
    ASSERT_TRUE(row.is_object());
    const bool hasPdtv = row.contains("pdtv") || row.contains("PDTV");
    const bool hasAll = row.contains("allpdtv1") || row.contains("AllPDTV1");
    EXPECT_TRUE(hasPdtv);
    EXPECT_TRUE(hasAll);
}

TEST_F(AutoPadSourceTest, PdtvRejectsNonCanonicalIdsAndKeepsSentinel)
{
    closeCachedConnections(prdArchive_);
    createPdtvArchive(prdArchive_, "file|dt|C:\\tmp\\file", "");

    GetIshTelegaPdtvCommand command;
    const std::vector<std::string> badRequests = {
        "",
        "-1",
        "2147483648",
        "12a",
        "1 OR 1=1",
        "1'; DELETE FROM sentinel; --",
        " 7",
        "7 ",
        "'7'",
    };

    for (const auto& request : badRequests) {
        const auto result = command.executeResult(bytesOf(request));
        ASSERT_TRUE(result.failed()) << request << " " << result.diagnostic;
        EXPECT_EQ(result.error, ErrorCode::InvalidRequest) << request;
        closeCachedConnections(prdArchive_);
        EXPECT_TRUE(sentinelExists(prdArchive_)) << request;
    }

    std::vector<std::uint8_t> embeddedNul{'7', 0, '1'};
    const auto nulResult = command.executeResult(embeddedNul);
    ASSERT_TRUE(nulResult.failed());
    EXPECT_EQ(nulResult.error, ErrorCode::InvalidRequest);
    closeCachedConnections(prdArchive_);
    EXPECT_TRUE(sentinelExists(prdArchive_));
}

