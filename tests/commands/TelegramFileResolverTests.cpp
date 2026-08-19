#include "AsioServer/AsioServer.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "Commands/TelegramFiles/TelegramFileResolver.h"
#include "MyUtils/Utf8Path.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/sqlite3.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;
    using command_execution::ErrorCode;

    constexpr std::uint64_t kLargeFileSize = 64ull * 1024 * 1024;

    std::vector<std::uint8_t> requestBytes(const nh::json& request)
    {
        const std::string serialized = request.dump();
        return {serialized.begin(), serialized.end()};
    }

    std::string readTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

    std::vector<std::uint8_t> readResolvedBytes(const ResolvedTelegramFile& file)
    {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.size));
        if (file.size > 0 && file.stream) {
            file.stream->read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(file.size));
            EXPECT_EQ(
                file.stream->gcount(),
                static_cast<std::streamsize>(file.size));
        }
        return bytes;
    }

    class TelegramFileResolverTest : public ::testing::Test
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
                ("searchengine-telegram-file-" + std::to_string(uniqueValue));
            filesDirectory_ = root_ / "files";
            databasePath_ = root_ / "ARCHIVE.db3";
            fs::create_directories(filesDirectory_);
            createArchiveTable();

            Telega::year = "2099";
            Telega::prm_base_dir = root_.string();
            Telega::prd_base_dir = root_.string();
            Telega::b_prm.clear();
            Telega::b_prd.clear();
        }

        void TearDown() override
        {
            Telega::b_prm.clear();
            Telega::b_prd.clear();
            Telega::prm_base_dir.clear();
            Telega::prd_base_dir.clear();
            SQLiteConnectionManager::instance().closeConnection(
                databasePath_.string());

            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }

        static void executeSql(sqlite3* database, const char* sql)
        {
            char* error = nullptr;
            const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
            const std::string message = error ? error : "";
            sqlite3_free(error);
            ASSERT_EQ(result, SQLITE_OK) << message;
        }

        void closeCachedDatabase()
        {
            SQLiteConnectionManager::instance().closeConnection(
                databasePath_.string());
            Telega::b_prm.clear();
            Telega::b_prd.clear();
        }

        sqlite3* openDatabase()
        {
            closeCachedDatabase();
            sqlite3* database = nullptr;
            EXPECT_EQ(
                sqlite3_open(databasePath_.string().c_str(), &database),
                SQLITE_OK);
            EXPECT_NE(database, nullptr);
            return database;
        }

        void createArchiveTable()
        {
            sqlite3* database = openDatabase();
            ASSERT_NE(database, nullptr);
            executeSql(
                database,
                "CREATE TABLE archive ("
                "`index` INTEGER PRIMARY KEY, PrilName TEXT, DirectTo TEXT, "
                "FileName TEXT)");
            ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
        }

        void insertRow(
            int id,
            const std::string& prilName,
            const std::string& directTo,
            const std::string& fileName)
        {
            sqlite3* database = openDatabase();
            ASSERT_NE(database, nullptr);

            sqlite3_stmt* statement = nullptr;
            ASSERT_EQ(
                sqlite3_prepare_v2(
                    database,
                    "INSERT OR REPLACE INTO archive "
                    "(`index`, PrilName, DirectTo, FileName) VALUES (?, ?, ?, ?)",
                    -1,
                    &statement,
                    nullptr),
                SQLITE_OK);

            ASSERT_EQ(sqlite3_bind_int(statement, 1, id), SQLITE_OK);
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    2,
                    prilName.c_str(),
                    -1,
                    SQLITE_TRANSIENT),
                SQLITE_OK);
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    3,
                    directTo.c_str(),
                    -1,
                    SQLITE_TRANSIENT),
                SQLITE_OK);
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    4,
                    fileName.c_str(),
                    -1,
                    SQLITE_TRANSIENT),
                SQLITE_OK);
            ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
            ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
            ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
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

        std::string directoryUtf8() const
        {
            return encoding::path_to_utf8(filesDirectory_);
        }

        fs::path root_;
        fs::path filesDirectory_;
        fs::path databasePath_;
        const std::vector<std::uint8_t> expectedContent_{0x00, 0x42, 0xff};
    };
}

TEST(TelegramStreamingContract, ProductionPathDoesNotSlurpAttachmentIntoVector)
{
    const fs::path repoRoot =
        fs::path(__FILE__).parent_path().parent_path().parent_path();
    const fs::path oldCommand =
        repoRoot /
        "src/Commands/GetTelegaSingleAttachment/GetTelegaSingleAttachmentCmd.cpp";
    const fs::path resolver =
        repoRoot / "src/Commands/TelegramFiles/TelegramFileResolver.cpp";
    const fs::path server = repoRoot / "src/AsioServer/AsioServer.cpp";

    EXPECT_FALSE(fs::exists(oldCommand));
    ASSERT_TRUE(fs::exists(resolver));
    ASSERT_TRUE(fs::exists(server));

    const auto resolverText = readTextFile(resolver);
    const auto serverText = readTextFile(server);

    EXPECT_EQ(resolverText.find("istreambuf_iterator"), std::string::npos);
    EXPECT_EQ(resolverText.find("CommandResult::success"), std::string::npos);
    EXPECT_EQ(serverText.find("GetTelegaSingleAttachmentCmd"), std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::GET_SINGLE_ATACHMENT]"),
        std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::GET_TELEGA_TEXT]"),
        std::string::npos);
    EXPECT_NE(serverText.find("COMMAND::GET_SINGLE_ATACHMENT"), std::string::npos);
    EXPECT_NE(serverText.find("COMMAND::GET_TELEGA_TEXT"), std::string::npos);
    EXPECT_NE(serverText.find("FileTransfer"), std::string::npos);
    EXPECT_NE(serverText.find("TelegramFileResolver::resolveAttachment"), std::string::npos);
    EXPECT_NE(serverText.find("TelegramFileResolver::resolveText"), std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::GETBINFILE]"),
        std::string::npos);
    EXPECT_NE(serverText.find("cmdMap[COMMAND::FILETEXT]"), std::string::npos);
    EXPECT_NE(serverText.find("rejectRawBinFileDownload"), std::string::npos);
    EXPECT_EQ(
        serverText.find("std::make_shared<std::ifstream>"),
        std::string::npos);
}

TEST(TelegramProtocolOrdinals, ScopedTelegramCommandsKeepStableWireValues)
{
    using asio_server::COMMAND;
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::FILETEXT), 2u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETBINFILE), 11u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_TELEGA_ATACHMENTS), 25u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_SINGLE_ATACHMENT), 26u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::ERROR_RESPONSE), 29u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::NEGOTIATE_PROTOCOL_V1), 30u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::AUTHENTICATE_V1), 31u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_TELEGA_TEXT), 32u);
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::GET_SINGLE_ATACHMENT));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::GET_TELEGA_TEXT));
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::GET_TELEGA_TEXT));
}

TEST_F(TelegramFileResolverTest, ResolvesAdvertisedVhodAttachment)
{
    writeBytes(filesDirectory_ / "first.bin", expectedContent_);
    insertRow(42, "first.bin;missing.bin", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "first.bin"}}));

    ASSERT_TRUE(result.succeeded());
    ASSERT_TRUE(result.file.has_value());
    EXPECT_EQ(result.file->size, expectedContent_.size());
    EXPECT_TRUE(result.file->stream);
    EXPECT_TRUE(result.file->stream->is_open());
    EXPECT_EQ(readResolvedBytes(*result.file), expectedContent_);
}

TEST_F(TelegramFileResolverTest, ResolvesIshodZipAttachment)
{
    const std::vector<std::uint8_t> zipBytes{'P', 'K', 0x03, 0x04};
    writeBytes(filesDirectory_ / "56.zip", zipBytes);
    insertRow(56, "56.zip", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 56}, {"type", 1}, {"file_name", "56.zip"}}));

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.file->size, zipBytes.size());
    EXPECT_EQ(readResolvedBytes(*result.file), zipBytes);
}

TEST_F(TelegramFileResolverTest, ResolvesUnicodeAttachmentBasename)
{
    const std::string utf8Name = "файл.bin";
    const auto filePath = filesDirectory_ / encoding::utf8_to_path(utf8Name);
    writeBytes(filePath, expectedContent_);
    insertRow(42, utf8Name, directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", utf8Name}}));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(readResolvedBytes(*result.file), expectedContent_);
}

TEST_F(TelegramFileResolverTest, ResolvesZeroByteAttachment)
{
    writeBytes(filesDirectory_ / "empty.bin", {});
    insertRow(42, "empty.bin", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "empty.bin"}}));

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.file->size, 0u);
    EXPECT_TRUE(readResolvedBytes(*result.file).empty());
}

TEST_F(TelegramFileResolverTest, RejectsMalformedAndInvalidAttachmentRequests)
{
    insertRow(42, "first.bin", directoryUtf8(), "");

    const auto malformed = TelegramFileResolver::resolveAttachment({'{'});
    ASSERT_TRUE(malformed.failed());
    EXPECT_EQ(malformed.error, ErrorCode::InvalidJson);

    const auto missingFields = TelegramFileResolver::resolveAttachment(
        requestBytes({{"id", 42}}));
    ASSERT_TRUE(missingFields.failed());
    EXPECT_EQ(missingFields.error, ErrorCode::InvalidRequest);

    const auto negativeId = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", -1}, {"type", 0}, {"file_name", "first.bin"}}));
    ASSERT_TRUE(negativeId.failed());
    EXPECT_EQ(negativeId.error, ErrorCode::InvalidRequest);

    const auto overflowId = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", static_cast<std::int64_t>(
                    (std::numeric_limits<int>::max)()) +
                1},
         {"type", 0},
         {"file_name", "first.bin"}}));
    ASSERT_TRUE(overflowId.failed());
    EXPECT_EQ(overflowId.error, ErrorCode::InvalidRequest);

    const auto invalidType = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 2}, {"file_name", "first.bin"}}));
    ASSERT_TRUE(invalidType.failed());
    EXPECT_EQ(invalidType.error, ErrorCode::InvalidRequest);
}

TEST_F(TelegramFileResolverTest, RejectsUnsafeAttachmentBasenames)
{
    insertRow(42, "first.bin;x.bin", directoryUtf8(), "");
    writeBytes(filesDirectory_ / "first.bin", expectedContent_);

    const std::string embeddedNul("a\0b", 3);
    const std::vector<std::string> unsafeNames{
        "../x.bin",
        "..\\x.bin",
        "C:\\x.bin",
        "C:x.bin",
        "\\x.bin",
        "/x.bin",
        "dir/x.bin",
        "dir\\x.bin",
        ".",
        "..",
        "abc:stream",
        embeddedNul,
        ""};

    for (const auto& name : unsafeNames) {
        nh::json request{{"id", 42}, {"type", 0}};
        request["file_name"] = name;
        const auto result =
            TelegramFileResolver::resolveAttachment(requestBytes(request));
        ASSERT_TRUE(result.failed()) << name;
        EXPECT_EQ(result.error, ErrorCode::InvalidRequest) << name;
    }
}

TEST_F(TelegramFileResolverTest, ReportsMissingOrUnadvertisedAttachment)
{
    writeBytes(filesDirectory_ / "first.bin", expectedContent_);
    insertRow(42, "first.bin;missing.bin", directoryUtf8(), "");

    const auto missing = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "missing.bin"}}));
    const auto unadvertised = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "secret.bin"}}));
    const auto noTelegram = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 999}, {"type", 0}, {"file_name", "first.bin"}}));

    ASSERT_TRUE(missing.failed());
    EXPECT_EQ(missing.error, ErrorCode::AttachmentNotFound);
    ASSERT_TRUE(unadvertised.failed());
    EXPECT_EQ(unadvertised.error, ErrorCode::AttachmentNotFound);
    ASSERT_TRUE(noTelegram.failed());
    EXPECT_EQ(noTelegram.error, ErrorCode::AttachmentNotFound);
}

TEST_F(TelegramFileResolverTest, RejectsDirectoryInsteadOfAttachmentFile)
{
    fs::create_directory(filesDirectory_ / "folder.bin");
    insertRow(42, "folder.bin", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "folder.bin"}}));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::FileOpenFailed);
}

TEST_F(TelegramFileResolverTest, RejectsAttachmentReparsePointWhenAvailable)
{
#ifdef _WIN32
    writeBytes(filesDirectory_ / "target.bin", expectedContent_);
    const fs::path linkPath = filesDirectory_ / "link.bin";
    const DWORD flags = 0x2; // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (!CreateSymbolicLinkW(
            linkPath.c_str(),
            (filesDirectory_ / "target.bin").c_str(),
            flags) &&
        !CreateSymbolicLinkW(
            linkPath.c_str(),
            (filesDirectory_ / "target.bin").c_str(),
            0)) {
        GTEST_SKIP() << "symlink creation is not permitted";
    }
    insertRow(42, "link.bin", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "link.bin"}}));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::FileOpenFailed);
#else
    GTEST_SKIP() << "reparse-point check is Windows-specific";
#endif
}

TEST_F(TelegramFileResolverTest, ResolvesTelegramTextThroughStream)
{
    const std::vector<std::uint8_t> text{'H', 'i', '\r', '\n', 0xD0, 0x90};
    writeBytes(filesDirectory_ / "message.txt", text);
    insertRow(70, "", directoryUtf8(), "message.txt");

    const auto result = TelegramFileResolver::resolveText(
        requestBytes({{"id", 70}, {"type", 0}}));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(result.file->size, text.size());
    EXPECT_EQ(readResolvedBytes(*result.file), text);
}

TEST_F(TelegramFileResolverTest, ResolvesUnicodeTextPath)
{
    const auto unicodeDir = filesDirectory_ / encoding::utf8_to_path("тексты");
    fs::create_directories(unicodeDir);
    const std::string fileName = "письмо.txt";
    const std::vector<std::uint8_t> text{0xD1, 0x82, 0xD0, 0xB5, 0xD0, 0xBA, 0xD1, 0x81, 0xD1, 0x82};
    writeBytes(unicodeDir / encoding::utf8_to_path(fileName), text);
    insertRow(71, "", encoding::path_to_utf8(unicodeDir), fileName);

    const auto result = TelegramFileResolver::resolveText(
        requestBytes({{"id", 71}, {"type", 0}, {"note", "ok"}}));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(readResolvedBytes(*result.file), text);
}

TEST_F(TelegramFileResolverTest, ResolvesZeroByteText)
{
    writeBytes(filesDirectory_ / "empty.txt", {});
    insertRow(72, "", directoryUtf8(), "empty.txt");

    const auto result = TelegramFileResolver::resolveText(
        requestBytes({{"id", 72}, {"type", 0}}));

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.file->size, 0u);
}

TEST_F(TelegramFileResolverTest, RejectsMalformedAndRawPathTextRequests)
{
    insertRow(70, "", directoryUtf8(), "message.txt");
    writeBytes(filesDirectory_ / "message.txt", {'x'});

    EXPECT_EQ(
        TelegramFileResolver::resolveText({'{'}).error,
        ErrorCode::InvalidJson);
    EXPECT_EQ(
        TelegramFileResolver::resolveText(requestBytes({{"id", 70}})).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", -3}, {"type", 0}}))
            .error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 70}, {"type", 9}}))
            .error,
        ErrorCode::InvalidRequest);

    for (const char* key : {"path", "dir", "DirectTo", "FileName", "remotePath"}) {
        nh::json request{{"id", 70}, {"type", 0}};
        request[key] = "C:/secret.txt";
        const auto result = TelegramFileResolver::resolveText(requestBytes(request));
        ASSERT_TRUE(result.failed()) << key;
        EXPECT_EQ(result.error, ErrorCode::InvalidRequest) << key;
    }
}

TEST_F(TelegramFileResolverTest, ReportsTextArchiveAndFilesystemErrors)
{
    writeBytes(filesDirectory_ / "message.txt", {'x'});
    insertRow(70, "", directoryUtf8(), "message.txt");

    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 999}, {"type", 0}}))
            .error,
        ErrorCode::FileNotFound);

    insertRow(73, "", "", "message.txt");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 73}, {"type", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    insertRow(74, "", directoryUtf8(), "");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 74}, {"type", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    insertRow(75, "", directoryUtf8(), "C:\\\\Windows\\\\x.txt");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 75}, {"type", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    insertRow(76, "", directoryUtf8(), "../secret.txt");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 76}, {"type", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    insertRow(77, "", directoryUtf8(), "missing.txt");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 77}, {"type", 0}}))
            .error,
        ErrorCode::FileNotFound);

    fs::create_directory(filesDirectory_ / "not-a-file.txt");
    insertRow(78, "", directoryUtf8(), "not-a-file.txt");
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 78}, {"type", 0}}))
            .error,
        ErrorCode::FileOpenFailed);
}

TEST_F(TelegramFileResolverTest, ReportsDisabledAndUnavailableTextSource)
{
    insertRow(70, "", directoryUtf8(), "message.txt");
    writeBytes(filesDirectory_ / "message.txt", {'x'});

    Telega::prm_base_dir.clear();
    Telega::b_prm.clear();
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 70}, {"type", 0}}))
            .error,
        ErrorCode::DataSourceDisabled);

    Telega::prm_base_dir = (root_ / "missing-source").string();
    Telega::b_prm.clear();
    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 70}, {"type", 0}}))
            .error,
        ErrorCode::DataSourceUnavailable);
}

TEST_F(TelegramFileResolverTest, ReportsSchemaErrorWhenFileNameColumnMissing)
{
    closeCachedDatabase();
    std::error_code ignored;
    fs::remove(databasePath_, ignored);
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(databasePath_.string().c_str(), &database), SQLITE_OK);
    executeSql(
        database,
        "CREATE TABLE archive (`index` INTEGER PRIMARY KEY, PrilName TEXT, "
        "DirectTo TEXT)");
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);

    EXPECT_EQ(
        TelegramFileResolver::resolveText(
            requestBytes({{"id", 70}, {"type", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);
    EXPECT_EQ(
        TelegramFileResolver::resolveAttachment(requestBytes(
            {{"id", 42}, {"type", 0}, {"file_name", "first.bin"}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);
}

TEST_F(TelegramFileResolverTest, LargeAttachmentUsesOpenStreamAndUint64Size)
{
    writeBytes(filesDirectory_ / "first.bin", expectedContent_);
    const fs::path largePath = filesDirectory_ / "large.bin";
    {
        std::ofstream file(largePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        std::vector<char> chunk(64 * 1024);
        for (std::uint64_t offset = 0; offset < kLargeFileSize; offset += chunk.size()) {
            for (std::size_t index = 0; index < chunk.size(); ++index)
                chunk[index] = static_cast<char>((offset + index) & 0xFF);
            file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        }
        ASSERT_TRUE(file.good());
    }
    insertRow(42, "large.bin", directoryUtf8(), "");

    const auto result = TelegramFileResolver::resolveAttachment(requestBytes(
        {{"id", 42}, {"type", 0}, {"file_name", "large.bin"}}));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.file.has_value());
    EXPECT_EQ(result.file->size, kLargeFileSize);
    EXPECT_TRUE(result.file->stream);
    EXPECT_TRUE(result.file->stream->is_open());

    std::uint64_t total = 0;
    std::vector<char> buffer(64 * 1024);
    while (total < result.file->size) {
        const auto next = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(buffer.size()),
                       result.file->size - total));
        result.file->stream->read(buffer.data(), static_cast<std::streamsize>(next));
        const auto got = result.file->stream->gcount();
        ASSERT_GT(got, 0);
        if (total == 0)
            EXPECT_EQ(static_cast<unsigned char>(buffer[0]), 0u);
        total += static_cast<std::uint64_t>(got);
    }
    EXPECT_EQ(total, kLargeFileSize);
}
