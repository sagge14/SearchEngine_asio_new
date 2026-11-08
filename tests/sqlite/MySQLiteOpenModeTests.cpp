#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/mySQLite.h"
#include "SQLite/sqlite3.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    fs::path makeUniqueRoot(const char* prefix)
    {
        static std::atomic_uint64_t sequence{0};
        const auto uniqueValue =
            std::chrono::steady_clock::now().time_since_epoch().count() +
            static_cast<std::int64_t>(
                sequence.fetch_add(1, std::memory_order_relaxed));
        auto root = fs::temp_directory_path() /
            (std::string(prefix) + std::to_string(uniqueValue));
        fs::create_directories(root);
        return root;
    }

    class UniqueTempDir
    {
    public:
        explicit UniqueTempDir(const char* prefix)
            : root_(makeUniqueRoot(prefix))
        {
        }

        ~UniqueTempDir()
        {
            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }

        UniqueTempDir(const UniqueTempDir&) = delete;
        UniqueTempDir& operator=(const UniqueTempDir&) = delete;

        [[nodiscard]] const fs::path& path() const { return root_; }

    private:
        fs::path root_;
    };
}

TEST(MySQLiteOpenMode, WritableOpenCreatesMissingDatabaseAndAllowsInsert)
{
    UniqueTempDir temp("searchengine-mysqlite-rw-");
    const auto databasePath = temp.path() / "writable.db";
    ASSERT_FALSE(fs::exists(databasePath));

    mySQLite database(databasePath.string());
    EXPECT_TRUE(fs::exists(databasePath));
    ASSERT_NO_THROW(
        database.execSql("CREATE TABLE t (id INTEGER); INSERT INTO t VALUES (1);"));

    const auto rows = database.queryRows("SELECT id FROM t");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().at("id"), "1");
}

TEST(MySQLiteOpenMode, ReadOnlyOpenDoesNotCreateMissingDatabase)
{
    UniqueTempDir temp("searchengine-mysqlite-ro-");
    const auto databasePath = temp.path() / "missing.db";
    ASSERT_FALSE(fs::exists(databasePath));

    EXPECT_THROW(
        mySQLite(databasePath.string(), mySQLite::OpenMode::ReadOnly),
        SQLiteOpenError);
    EXPECT_FALSE(fs::exists(databasePath));
}

TEST(MySQLiteOpenMode, QueryErrorsDistinguishSchemaFromOtherSqlFailures)
{
    UniqueTempDir temp("searchengine-mysqlite-query-");
    const auto databasePath = temp.path() / "schema.db";

    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &raw), SQLITE_OK);
        char* error = nullptr;
        ASSERT_EQ(
            sqlite3_exec(raw, "CREATE TABLE t (id INTEGER)", nullptr, nullptr, &error),
            SQLITE_OK)
            << (error ? error : "");
        sqlite3_free(error);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }

    mySQLite database(databasePath.string(), mySQLite::OpenMode::ReadOnly);

    try {
        database.execSql("SELECT * FROM missing_table");
        FAIL() << "expected SQLiteQueryError for missing table";
    }
    catch (const SQLiteQueryError& error) {
        EXPECT_TRUE(error.isSchemaFailure());
        EXPECT_NE(error.path().find("schema.db"), std::string::npos);
    }

    try {
        database.execSql("THIS IS NOT SQL");
        FAIL() << "expected SQLiteQueryError for invalid SQL";
    }
    catch (const SQLiteQueryError& error) {
        EXPECT_FALSE(error.isSchemaFailure());
    }
}

TEST(MySQLiteOpenMode, ReadOnlyCacheDoesNotReplaceWritableConnection)
{
    UniqueTempDir temp("searchengine-mysqlite-cache-");
    const auto databasePath = temp.path() / "shared.db";

    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(databasePath.string().c_str(), &raw), SQLITE_OK);
        char* error = nullptr;
        ASSERT_EQ(
            sqlite3_exec(raw, "CREATE TABLE t (id INTEGER)", nullptr, nullptr, &error),
            SQLITE_OK)
            << (error ? error : "");
        sqlite3_free(error);
        ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    }

    auto& manager = SQLiteConnectionManager::instance();
    auto readOnly = manager.getReadOnlyConnection(databasePath.string());
    auto writable = manager.getConnection(databasePath.string());
    ASSERT_NE(readOnly.get(), writable.get());

    ASSERT_NO_THROW(writable->execSql("INSERT INTO t(id) VALUES (7);"));
    const auto rows = writable->queryRows("SELECT id FROM t");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().at("id"), "7");

    manager.closeConnection(databasePath.string());
}
