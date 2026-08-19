#include "AsioServer/AsioServer.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "Commands/TelegramFiles/PdtvFileResolver.h"
#include "MyUtils/Utf8Path.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/sqlite3.h"
#include "nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

    class PdtvFileResolverTest : public ::testing::Test
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
                ("searchengine-pdtv-file-" + std::to_string(uniqueValue));
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
                "`index` INTEGER PRIMARY KEY, pdtv TEXT, allpdtv1 TEXT)");
            ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
        }

        void insertRow(
            int id,
            const std::string& pdtv,
            const std::string& allPdtv1)
        {
            sqlite3* database = openDatabase();
            ASSERT_NE(database, nullptr);

            sqlite3_stmt* statement = nullptr;
            ASSERT_EQ(
                sqlite3_prepare_v2(
                    database,
                    "INSERT OR REPLACE INTO archive "
                    "(`index`, pdtv, allpdtv1) VALUES (?, ?, ?)",
                    -1,
                    &statement,
                    nullptr),
                SQLITE_OK);

            ASSERT_EQ(sqlite3_bind_int(statement, 1, id), SQLITE_OK);
            ASSERT_EQ(
                sqlite3_bind_text(
                    statement,
                    2,
                    pdtv.c_str(),
                    -1,
                    SQLITE_TRANSIENT),
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

        std::string pathUtf8(const fs::path& path) const
        {
            return encoding::path_to_utf8(path);
        }

        std::string makeEntry(
            const std::string& file,
            const std::string& dateTime,
            const fs::path& path,
            const std::string& suffix = {}) const
        {
            return file + "|" + dateTime + "|" + pathUtf8(path) + suffix;
        }

        fs::path root_;
        fs::path filesDirectory_;
        fs::path databasePath_;
        const std::vector<std::uint8_t> expectedContent_{'P', 'D', 'T', 'V', 0x0D};
    };
}

TEST(PdtvStreamingContract, ProductionPathDoesNotSlurpConfirmationIntoVector)
{
    const fs::path repoRoot =
        fs::path(__FILE__).parent_path().parent_path().parent_path();
    const fs::path resolver =
        repoRoot / "src/Commands/TelegramFiles/PdtvFileResolver.cpp";
    const fs::path server = repoRoot / "src/AsioServer/AsioServer.cpp";

    ASSERT_TRUE(fs::exists(resolver));
    ASSERT_TRUE(fs::exists(server));

    const auto resolverText = readTextFile(resolver);
    const auto serverText = readTextFile(server);

    EXPECT_EQ(resolverText.find("istreambuf_iterator"), std::string::npos);
    EXPECT_EQ(resolverText.find("CommandResult::success"), std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::GET_ISH_PDTV_TEXT]"),
        std::string::npos);
    EXPECT_NE(serverText.find("cmdMap[COMMAND::FILETEXT]"), std::string::npos);
    EXPECT_NE(serverText.find("COMMAND::GET_ISH_PDTV_TEXT"), std::string::npos);
    EXPECT_NE(serverText.find("PdtvFileResolver::resolve"), std::string::npos);
    EXPECT_NE(serverText.find("FileTransfer"), std::string::npos);
    EXPECT_NE(serverText.find("rejectRawBinFileDownload"), std::string::npos);
}

TEST(PdtvProtocolOrdinals, ScopedPdtvCommandKeepsStableWireValue)
{
    using asio_server::COMMAND;
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::FILETEXT), 2u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETBINFILE), 11u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_ISH_PDTV), 24u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_TELEGA_TEXT), 32u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_ISH_PDTV_TEXT), 33u);
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::GET_ISH_PDTV_TEXT));
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::GET_ISH_PDTV_TEXT));
}

TEST_F(PdtvFileResolverTest, RejectsMalformedJson)
{
    const auto result = PdtvFileResolver::resolve(
        std::vector<std::uint8_t>{'{', 'b', 'a', 'd'});
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::InvalidJson);
}

TEST_F(PdtvFileResolverTest, RejectsBadSelectorFields)
{
    EXPECT_EQ(
        PdtvFileResolver::resolve(requestBytes({{"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", -1}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 1}, {"slot", 2}, {"entry_index", 0}}))
            .error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 1}, {"slot", 0}, {"entry_index", -1}}))
            .error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 1}, {"slot", 0}, {"entry_index", 1}}))
            .error,
        ErrorCode::InvalidRequest);
}

TEST_F(PdtvFileResolverTest, RejectsRawFilesystemFields)
{
    const std::vector<const char*> forbidden = {
        "path",
        "dir",
        "DirectTo",
        "FileName",
        "remotePath"};
    for (const char* key : forbidden) {
        const auto result = PdtvFileResolver::resolve(requestBytes(
            {{"id", 1}, {"slot", 0}, {"entry_index", 0}, {key, "C:\\\\tmp\\\\x"}}));
        ASSERT_TRUE(result.failed()) << key;
        EXPECT_EQ(result.error, ErrorCode::InvalidRequest) << key;
    }
}

TEST_F(PdtvFileResolverTest, ResolvesSinglePdtvEntry)
{
    const auto filePath = filesDirectory_ / "confirm.txt";
    writeBytes(filePath, expectedContent_);
    insertRow(11, makeEntry("confirm.txt", "20240101", filePath), "");

    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 11}, {"slot", 0}, {"entry_index", 0}}));

    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(result.file->size, expectedContent_.size());
    EXPECT_EQ(readResolvedBytes(*result.file), expectedContent_);
}

TEST_F(PdtvFileResolverTest, ResolvesAllPdtv1FirstMiddleLast)
{
    const auto first = filesDirectory_ / "first.txt";
    const auto middle = filesDirectory_ / "middle.txt";
    const auto last = filesDirectory_ / "last.txt";
    writeBytes(first, {'A'});
    writeBytes(middle, {'B', 'B'});
    writeBytes(last, {'C', 'C', 'C'});

    const auto all =
        makeEntry("first.txt", "d1", first) + "/" +
        makeEntry("middle.txt", "d2", middle) + "/" +
        makeEntry("last.txt", "d3", last);
    insertRow(22, "", all);

    const auto firstResult = PdtvFileResolver::resolve(
        requestBytes({{"id", 22}, {"slot", 1}, {"entry_index", 0}}));
    const auto middleResult = PdtvFileResolver::resolve(
        requestBytes({{"id", 22}, {"slot", 1}, {"entry_index", 1}}));
    const auto lastResult = PdtvFileResolver::resolve(
        requestBytes({{"id", 22}, {"slot", 1}, {"entry_index", 2}}));

    ASSERT_TRUE(firstResult.succeeded()) << firstResult.diagnostic;
    ASSERT_TRUE(middleResult.succeeded()) << middleResult.diagnostic;
    ASSERT_TRUE(lastResult.succeeded()) << lastResult.diagnostic;
    EXPECT_EQ(readResolvedBytes(*firstResult.file), (std::vector<std::uint8_t>{'A'}));
    EXPECT_EQ(readResolvedBytes(*middleResult.file), (std::vector<std::uint8_t>{'B', 'B'}));
    EXPECT_EQ(readResolvedBytes(*lastResult.file), (std::vector<std::uint8_t>{'C', 'C', 'C'}));
}

TEST_F(PdtvFileResolverTest, RejectsAllPdtv1OutOfRange)
{
    const auto filePath = filesDirectory_ / "only.txt";
    writeBytes(filePath, expectedContent_);
    insertRow(22, "", makeEntry("only.txt", "d1", filePath));

    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 22}, {"slot", 1}, {"entry_index", 1}}));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::InvalidRequest);
}

TEST_F(PdtvFileResolverTest, RejectsMalformedAndMissingPathMetadata)
{
    insertRow(31, "not-a-pdtv-entry", "");
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 31}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    insertRow(32, "file|date|", "");
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 32}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::DatabaseSchemaFailed);

    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 999}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::FileNotFound);
}

TEST_F(PdtvFileResolverTest, NormalizesLegacySemicolonAndTrailingSlash)
{
    const auto filePath = filesDirectory_ / "legacy.txt";
    writeBytes(filePath, expectedContent_);
    insertRow(
        41,
        makeEntry("legacy.txt", "d", filePath, "/;junk"),
        "");

    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 41}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(readResolvedBytes(*result.file), expectedContent_);
}

TEST_F(PdtvFileResolverTest, RejectsMissingFileAndDirectory)
{
    const auto missing = filesDirectory_ / "gone.txt";
    insertRow(51, makeEntry("gone.txt", "d", missing), "");
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 51}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::FileNotFound);

    const auto directory = filesDirectory_ / "folder";
    fs::create_directory(directory);
    insertRow(52, makeEntry("folder", "d", directory), "");
    EXPECT_EQ(
        PdtvFileResolver::resolve(
            requestBytes({{"id", 52}, {"slot", 0}, {"entry_index", 0}}))
            .error,
        ErrorCode::FileOpenFailed);
}

TEST_F(PdtvFileResolverTest, ResolvesUnicodePathAndZeroSizeFile)
{
    const auto unicodeDir = filesDirectory_ / encoding::utf8_to_path("подтверждения");
    fs::create_directories(unicodeDir);
    const auto unicodeFile = unicodeDir / encoding::utf8_to_path("файл.txt");
    const std::vector<std::uint8_t> text{0xD0, 0x9F, 0xD0, 0x94};
    writeBytes(unicodeFile, text);
    insertRow(61, makeEntry("файл.txt", "d", unicodeFile), "");

    const auto unicodeResult = PdtvFileResolver::resolve(
        requestBytes({{"id", 61}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(unicodeResult.succeeded()) << unicodeResult.diagnostic;
    EXPECT_EQ(readResolvedBytes(*unicodeResult.file), text);

    const auto emptyFile = filesDirectory_ / "empty.txt";
    writeBytes(emptyFile, {});
    insertRow(62, makeEntry("empty.txt", "d", emptyFile), "");
    const auto emptyResult = PdtvFileResolver::resolve(
        requestBytes({{"id", 62}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(emptyResult.succeeded()) << emptyResult.diagnostic;
    EXPECT_EQ(emptyResult.file->size, 0u);
    EXPECT_TRUE(emptyResult.file->stream);
    EXPECT_TRUE(emptyResult.file->stream->is_open());
}

TEST_F(PdtvFileResolverTest, RejectsReparsePointWhenAvailable)
{
#ifdef _WIN32
    writeBytes(filesDirectory_ / "target.txt", expectedContent_);
    const fs::path linkPath = filesDirectory_ / "link.txt";
    const DWORD flags = 0x2; // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    if (!CreateSymbolicLinkW(
            linkPath.c_str(),
            (filesDirectory_ / "target.txt").c_str(),
            flags) &&
        !CreateSymbolicLinkW(
            linkPath.c_str(),
            (filesDirectory_ / "target.txt").c_str(),
            0)) {
        GTEST_SKIP() << "symlink creation is not permitted";
    }
    insertRow(71, makeEntry("link.txt", "d", linkPath), "");

    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 71}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::FileOpenFailed);
#else
    GTEST_SKIP() << "reparse-point check is Windows-specific";
#endif
}

TEST_F(PdtvFileResolverTest, DisabledPrdReturnsDataSourceDisabled)
{
    Telega::prd_base_dir.clear();
    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 1}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceDisabled);
}

TEST_F(PdtvFileResolverTest, MissingPrdSourceReturnsDataSourceUnavailable)
{
    Telega::prd_base_dir = (root_ / "missing-prd").string();
    const auto result = PdtvFileResolver::resolve(
        requestBytes({{"id", 1}, {"slot", 0}, {"entry_index", 0}}));
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::DataSourceUnavailable);
}
