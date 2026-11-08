#include <gtest/gtest.h>

#include "Auth/AuthClientStore.h"
#include "SQLite/sqlite3.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
    class TempPath
    {
    public:
        TempPath()
            : path_(std::filesystem::temp_directory_path() /
                    ("se_auth_store_" +
                     std::to_string(
                         static_cast<unsigned long long>(
                             reinterpret_cast<std::uintptr_t>(this))) +
                     ".sqlite"))
        {
            std::filesystem::remove(path_);
        }

        ~TempPath()
        {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    void execSql(sqlite3* db, const char* sql)
    {
        char* error = nullptr;
        const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        std::string detail = error ? error : "";
        sqlite3_free(error);
        ASSERT_EQ(result, SQLITE_OK) << detail;
    }
}

TEST(AuthClientStore, NewDatabaseUsesSchemaV2)
{
    TempPath temp;
    auth::AuthClientStore store;
    store.open(temp.path());
    store.upsertClient("usb-1", "desk-a", "usb", "USB-SERIAL-1", true);
    store.upsertClient(
        "pc-1",
        "desk-pc",
        "computer",
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
        true);

    const auto usb = store.getClient("usb-1");
    ASSERT_TRUE(usb.has_value());
    EXPECT_EQ(usb->device_type, "usb");
    EXPECT_EQ(usb->device_id, "USB-SERIAL-1");

    const auto pc = store.getClient("pc-1");
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(pc->device_type, "computer");
    EXPECT_EQ(pc->device_id, "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
    store.close();

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(temp.path().string().c_str(), &db), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), auth::kAuthClientsSchemaUserVersion);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST(AuthClientStore, RejectsLegacyFlashSerialSchema)
{
    TempPath temp;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(temp.path().string().c_str(), &db), SQLITE_OK);
    execSql(
        db,
        "CREATE TABLE clients ("
        "  client_id TEXT PRIMARY KEY NOT NULL,"
        "  client_name TEXT NOT NULL,"
        "  flash_serial TEXT NOT NULL,"
        "  enabled INTEGER NOT NULL DEFAULT 1,"
        "  signature_meta TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");");
    execSql(db, "PRAGMA user_version = 1;");
    sqlite3_close(db);

    auth::AuthClientStore store;
    try {
        store.open(temp.path());
        FAIL() << "expected incompatible schema error";
    } catch (const std::runtime_error& ex) {
        const std::string message = ex.what();
        EXPECT_NE(message.find("incompatible auth_clients.sqlite schema"), std::string::npos);
        EXPECT_NE(message.find("AuthDbTool"), std::string::npos);
    }
}
