#include "Commands/CommandResult.h"
#include "Commands/GetTelegaWay/GetTelegaWayCmd.h"
#include "Commands/GetTelegaWay/TelegaWay.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/sqlite3.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
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

    std::string payloadText(const command_execution::CommandResult& result)
    {
        return {result.payload.begin(), result.payload.end()};
    }

    void executeSql(sqlite3* database, const char* sql)
    {
        char* error = nullptr;
        const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        ASSERT_EQ(result, SQLITE_OK) << message;
    }

    void createYearDatabase(const fs::path& databasePath, bool withWayTable, bool withRows)
    {
        sqlite3* database = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &database), SQLITE_OK);
        ASSERT_NE(database, nullptr);

        if (withWayTable) {
            executeSql(
                database,
                "CREATE TABLE way ("
                "type TEXT, number TEXT, ddate TEXT, ttime TEXT, "
                "kuda TEXT, ind TEXT, str TEXT, pos TEXT)");
            if (withRows) {
                executeSql(
                    database,
                    "INSERT INTO way VALUES "
                    "('1','100','01.01.2026','10:00:00','A','1','s','p'),"
                    "('2','200','02.01.2026','11:00:00','B','2','s2','p2')");
            }
        }
        else {
            executeSql(database, "CREATE TABLE other (id INTEGER)");
        }

        ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    }

    void createBaseDatabase(const fs::path& databasePath, bool withTabTable, bool withRows)
    {
        sqlite3* database = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &database), SQLITE_OK);
        ASSERT_NE(database, nullptr);

        if (withTabTable) {
            executeSql(
                database,
                "CREATE TABLE tab ("
                "type TEXT, print TEXT, number TEXT, Sr TEXT, ind TEXT, GdeSht TEXT)");
            if (withRows) {
                executeSql(
                    database,
                    "INSERT INTO tab VALUES "
                    "('1','0','100','','5','ROOM1'),"
                    "('2','0','200','','6','ROOM2')");
            }
        }
        else {
            executeSql(database, "CREATE TABLE other (id INTEGER)");
        }

        ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    }

    class TelegaWayCommandResultTest : public ::testing::Test
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
                ("searchengine-telega-way-" + std::to_string(uniqueValue));
            fs::create_directories(root_);

            yearDb_ = root_ / "2099.db";
            baseDb_ = root_ / "base.db";

            previousWayDir_ = TelegaWay::base_way_dir;
            previousF12Dir_ = TelegaWay::base_f12_dir;
            previousYear_ = TelegaWay::work_year;

            TelegaWay::base_way_dir = yearDb_.string();
            TelegaWay::base_f12_dir = baseDb_.string();
            TelegaWay::work_year = "2099";
        }

        void TearDown() override
        {
            SQLiteConnectionManager::instance().closeConnection(yearDb_.string());
            SQLiteConnectionManager::instance().closeConnection(baseDb_.string());

            TelegaWay::base_way_dir = previousWayDir_;
            TelegaWay::base_f12_dir = previousF12Dir_;
            TelegaWay::work_year = previousYear_;

            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }

        fs::path root_;
        fs::path yearDb_;
        fs::path baseDb_;
        std::string previousWayDir_;
        std::string previousF12Dir_;
        std::string previousYear_;
    };
}

TEST_F(TelegaWayCommandResultTest, MissingYearDatabaseReturnsDataSourceUnavailableWithoutCreatingFile)
{
    ASSERT_FALSE(fs::exists(yearDb_));
    ASSERT_FALSE(fs::exists(baseDb_));

    GetTelegaWayVhCmd vhCommand;
    const auto vh = vhCommand.executeResult(bytesOf("100"));
    GetTelegaWayIshCmd ishCommand;
    const auto ish = ishCommand.executeResult(bytesOf("200"));

    ASSERT_TRUE(vh.failed());
    EXPECT_EQ(vh.error, ErrorCode::DataSourceUnavailable);
    EXPECT_NE(vh.error, ErrorCode::CommandExecutionFailed);
    EXPECT_NE(vh.diagnostic.find(yearDb_.string()), std::string::npos);
    ASSERT_TRUE(ish.failed());
    EXPECT_EQ(ish.error, ErrorCode::DataSourceUnavailable);
    EXPECT_FALSE(fs::exists(yearDb_));
    EXPECT_FALSE(fs::exists(baseDb_));
}

TEST_F(TelegaWayCommandResultTest, MissingBaseDatabaseReturnsDataSourceUnavailableWithoutCreatingFile)
{
    createYearDatabase(yearDb_, true, true);
    ASSERT_TRUE(fs::exists(yearDb_));
    ASSERT_FALSE(fs::exists(baseDb_));

    GetTelegaWayVhCmd command;
    const auto result = command.executeResult(bytesOf("100"));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
    EXPECT_NE(result.error, ErrorCode::CommandExecutionFailed);
    EXPECT_NE(result.diagnostic.find(baseDb_.string()), std::string::npos);
    EXPECT_FALSE(fs::exists(baseDb_));
}

TEST_F(TelegaWayCommandResultTest, SuccessfulVhAndIshQueriesPreserveWayRowsAndCurrentLocation)
{
    createYearDatabase(yearDb_, true, true);
    createBaseDatabase(baseDb_, true, true);

    GetTelegaWayVhCmd vhCommand;
    const auto vh = vhCommand.executeResult(bytesOf("100"));
    ASSERT_TRUE(vh.succeeded()) << vh.diagnostic;
    const auto vhJson = nh::json::parse(payloadText(vh));
    ASSERT_TRUE(vhJson.is_array());
    ASSERT_EQ(vhJson.size(), 2u);
    EXPECT_EQ(vhJson.at(0).at("number"), "100");
    EXPECT_EQ(vhJson.at(0).at("kuda"), "A");
    EXPECT_EQ(vhJson.at(0).at("type"), "1");
    EXPECT_EQ(vhJson.at(1).at("number"), "100");
    EXPECT_EQ(vhJson.at(1).at("kuda"), "L-k:ROOM1");
    EXPECT_EQ(vhJson.at(1).at("type"), "1");
    EXPECT_EQ(vhJson.at(1).at("ind"), "0");

    GetTelegaWayIshCmd ishCommand;
    const auto ish = ishCommand.executeResult(bytesOf("200"));
    ASSERT_TRUE(ish.succeeded()) << ish.diagnostic;
    const auto ishJson = nh::json::parse(payloadText(ish));
    ASSERT_TRUE(ishJson.is_array());
    ASSERT_EQ(ishJson.size(), 2u);
    EXPECT_EQ(ishJson.at(0).at("number"), "200");
    EXPECT_EQ(ishJson.at(0).at("kuda"), "B");
    EXPECT_EQ(ishJson.at(0).at("type"), "2");
    EXPECT_EQ(ishJson.at(1).at("kuda"), "L-k:ROOM2");
    EXPECT_EQ(ishJson.at(1).at("type"), "2");
}

TEST_F(TelegaWayCommandResultTest, MissingWayTableReturnsDatabaseSchemaFailed)
{
    createYearDatabase(yearDb_, false, false);
    createBaseDatabase(baseDb_, true, true);

    GetTelegaWayVhCmd command;
    const auto result = command.executeResult(bytesOf("100"));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DatabaseSchemaFailed);
    EXPECT_NE(result.error, ErrorCode::CommandExecutionFailed);
    EXPECT_NE(result.diagnostic.find("no such table"), std::string::npos);
    EXPECT_NE(result.diagnostic.find(yearDb_.string()), std::string::npos);
}

TEST_F(TelegaWayCommandResultTest, MissingTabTableReturnsDatabaseSchemaFailed)
{
    createYearDatabase(yearDb_, true, true);
    createBaseDatabase(baseDb_, false, false);

    GetTelegaWayVhCmd command;
    const auto result = command.executeResult(bytesOf("100"));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DatabaseSchemaFailed);
    EXPECT_NE(result.error, ErrorCode::CommandExecutionFailed);
    EXPECT_NE(result.diagnostic.find("no such table"), std::string::npos);
    EXPECT_NE(result.diagnostic.find(baseDb_.string()), std::string::npos);
}
