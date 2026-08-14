#include "Commands/GetJsonTelega/Telega.h"
#include "Commands/GetTelegaAttachments/GetTelegaAttachments.h"
#include "Commands/GetTelegaSingleAttachment/GetTelegaSingleAttachmentCmd.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/sqlite3.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;

    std::vector<std::uint8_t> requestBytes(const nh::json& request)
    {
        const std::string serialized = request.dump();
        return {serialized.begin(), serialized.end()};
    }

    class TelegaAttachmentCommandResultTest : public ::testing::Test
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
                ("searchengine-telega-attachment-" +
                 std::to_string(uniqueValue));
            attachmentDirectory_ = root_ / "attachments";
            databasePath_ = root_ / "archive.db3";
            invalidDatabasePath_ = root_ / "invalid-schema.db3";
            fs::create_directories(attachmentDirectory_);

            writeBytes(attachmentDirectory_ / "first.bin", expectedContent_);
            createArchiveDatabase();
            createEmptyDatabase(invalidDatabasePath_);

            Telega::b_prm = {databasePath_.string()};
            Telega::b_prd = {databasePath_.string()};
        }

        void TearDown() override
        {
            Telega::b_prm.clear();
            Telega::b_prd.clear();
            SQLiteConnectionManager::instance().closeConnection(
                databasePath_.string());
            SQLiteConnectionManager::instance().closeConnection(
                invalidDatabasePath_.string());

            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }

        static void writeBytes(
            const fs::path& path,
            const std::vector<std::uint8_t>& bytes)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(file.is_open());
            if (!bytes.empty()) {
                file.write(
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            }
            ASSERT_TRUE(file.good());
        }

        static void executeSql(sqlite3* database, const char* sql)
        {
            char* error = nullptr;
            const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
            const std::string message = error ? error : "";
            sqlite3_free(error);
            ASSERT_EQ(result, SQLITE_OK) << message;
        }

        static void createEmptyDatabase(const fs::path& path)
        {
            sqlite3* database = nullptr;
            ASSERT_EQ(sqlite3_open(path.string().c_str(), &database), SQLITE_OK);
            ASSERT_NE(database, nullptr);
            ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
        }

        void createArchiveDatabase()
        {
            sqlite3* database = nullptr;
            ASSERT_EQ(
                sqlite3_open(databasePath_.string().c_str(), &database),
                SQLITE_OK);
            ASSERT_NE(database, nullptr);

            executeSql(
                database,
                "CREATE TABLE archive ("
                "`index` INTEGER PRIMARY KEY, PrilName TEXT, DirectTo TEXT)");

            sqlite3_stmt* statement = nullptr;
            ASSERT_EQ(
                sqlite3_prepare_v2(
                    database,
                    "INSERT INTO archive (`index`, PrilName, DirectTo) "
                    "VALUES (?, ?, ?)",
                    -1,
                    &statement,
                    nullptr),
                SQLITE_OK);
            ASSERT_NE(statement, nullptr);

            ASSERT_EQ(sqlite3_bind_int(statement, 1, 42), SQLITE_OK);
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    2,
                    "first.bin;missing.bin",
                    -1,
                    SQLITE_STATIC),
                SQLITE_OK);
            const std::string directory = attachmentDirectory_.string();
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    3,
                    directory.c_str(),
                    -1,
                    SQLITE_TRANSIENT),
                SQLITE_OK);
            ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
            ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
            ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
        }

        fs::path root_;
        fs::path attachmentDirectory_;
        fs::path databasePath_;
        fs::path invalidDatabasePath_;
        const std::vector<std::uint8_t> expectedContent_{0x00, 0x42, 0xff};
    };
}

TEST_F(TelegaAttachmentCommandResultTest, RejectsMalformedJson)
{
    GetTelegaAttachmentsCmd listCommand;
    GetTelegaSingleAttachmentCmd singleCommand;

    const auto listResult = listCommand.executeResult({'{'});
    const auto singleResult = singleCommand.executeResult({'{'});

    ASSERT_TRUE(listResult.failed());
    EXPECT_EQ(listResult.error, command_execution::ErrorCode::InvalidJson);
    ASSERT_TRUE(singleResult.failed());
    EXPECT_EQ(singleResult.error, command_execution::ErrorCode::InvalidJson);
}

TEST_F(TelegaAttachmentCommandResultTest, RejectsInvalidFieldsAndTraversal)
{
    GetTelegaAttachmentsCmd listCommand;
    GetTelegaSingleAttachmentCmd singleCommand;

    const auto invalidList = listCommand.executeResult(
        requestBytes({{"id", -1}, {"type", 0}}));
    const auto traversal = singleCommand.executeResult(
        requestBytes({{"id", 42}, {"type", 0}, {"file_name", "../first.bin"}}));

    ASSERT_TRUE(invalidList.failed());
    EXPECT_EQ(invalidList.error, command_execution::ErrorCode::InvalidRequest);
    ASSERT_TRUE(traversal.failed());
    EXPECT_EQ(traversal.error, command_execution::ErrorCode::InvalidRequest);
}

TEST_F(TelegaAttachmentCommandResultTest, ListsExistingAndMissingFiles)
{
    GetTelegaAttachmentsCmd command;

    const auto result = command.executeResult(
        requestBytes({{"id", 42}, {"type", 0}}));

    ASSERT_TRUE(result.succeeded());
    const auto response = nh::json::parse(result.payload);
    ASSERT_EQ(response.size(), 2u);
    EXPECT_EQ(response[0]["name"], "first.bin");
    EXPECT_TRUE(response[0]["exists"].get<bool>());
    EXPECT_EQ(response[0]["size"], expectedContent_.size());
    EXPECT_EQ(response[1]["name"], "missing.bin");
    EXPECT_FALSE(response[1]["exists"].get<bool>());
    EXPECT_EQ(response[1]["size"], 0u);
}

TEST_F(TelegaAttachmentCommandResultTest, EmptyListIsSuccessfulJsonArray)
{
    GetTelegaAttachmentsCmd command;

    const auto result = command.executeResult(
        requestBytes({{"id", 999}, {"type", 0}}));

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(std::string(result.payload.begin(), result.payload.end()), "[]");
}

TEST_F(TelegaAttachmentCommandResultTest, LoadsAdvertisedAttachment)
{
    GetTelegaSingleAttachmentCmd command;

    const auto result = command.executeResult(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "first.bin"}}));

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.payload, expectedContent_);
}

TEST_F(TelegaAttachmentCommandResultTest, ReportsMissingOrUnadvertisedAttachment)
{
    GetTelegaSingleAttachmentCmd command;

    const auto missing = command.executeResult(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "missing.bin"}}));
    const auto unadvertised = command.executeResult(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "secret.bin"}}));
    const auto noTelegram = command.executeResult(requestBytes(
        {{"id", 999}, {"type", 0}, {"file_name", "first.bin"}}));

    ASSERT_TRUE(missing.failed());
    EXPECT_EQ(missing.error, command_execution::ErrorCode::AttachmentNotFound);
    ASSERT_TRUE(unadvertised.failed());
    EXPECT_EQ(
        unadvertised.error,
        command_execution::ErrorCode::AttachmentNotFound);
    ASSERT_TRUE(noTelegram.failed());
    EXPECT_EQ(noTelegram.error, command_execution::ErrorCode::AttachmentNotFound);
}

TEST_F(TelegaAttachmentCommandResultTest, ReportsDatabaseQueryFailure)
{
    Telega::b_prm = {invalidDatabasePath_.string()};
    GetTelegaAttachmentsCmd command;

    const auto result = command.executeResult(
        requestBytes({{"id", 42}, {"type", 0}}));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::DatabaseQueryFailed);
}

TEST_F(TelegaAttachmentCommandResultTest, QueryRowsReturnsStableSnapshot)
{
    auto database = SQLiteConnectionManager::instance().getConnection(
        databasePath_.string());

    const auto first = database->queryRows(
        "SELECT PrilName, DirectTo FROM archive WHERE `index` = 42");
    const auto second = database->queryRows(
        "SELECT PrilName, DirectTo FROM archive WHERE `index` = 999");

    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().at("PrilName"), "first.bin;missing.bin");
    EXPECT_TRUE(second.empty());
}
