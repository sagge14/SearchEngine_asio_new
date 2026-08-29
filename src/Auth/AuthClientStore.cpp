#include "Auth/AuthClientStore.h"

#include "Auth/DeviceIdentity.h"
#include "SQLite/sqlite3.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace auth
{
    namespace
    {
        constexpr const char* kIncompatibleSchemaMessage =
            "incompatible auth_clients.sqlite schema (expected device_type/"
            "device_id and PRAGMA user_version=2). Delete this database and "
            "recreate it with AuthDbTool after issuing new USB/computer "
            "device tokens.";

        [[noreturn]] void throwSqlite(
            sqlite3* db,
            const std::string& action,
            int code)
        {
            const char* detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
            throw std::runtime_error(
                action + " failed: SQLite " + std::to_string(code) +
                " (" + (detail ? detail : "unknown") + ')');
        }

        AuthClientRecord readClientRow(sqlite3_stmt* statement)
        {
            AuthClientRecord record;
            const auto* id = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0));
            const auto* name = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 1));
            const auto* type = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 2));
            const auto* device = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 3));
            const auto* meta = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 5));
            record.client_id = id ? id : "";
            record.client_name = name ? name : "";
            record.device_type = type ? type : "";
            record.device_id = device ? device : "";
            record.enabled = sqlite3_column_int(statement, 4) != 0;
            record.signature_meta = meta ? meta : "";
            record.created_at = sqlite3_column_int64(statement, 6);
            record.updated_at = sqlite3_column_int64(statement, 7);
            return record;
        }

        bool hasColumn(
            const std::vector<std::string>& columns,
            const char* name)
        {
            for (const auto& column : columns) {
                if (column == name) {
                    return true;
                }
            }
            return false;
        }
    }

    AuthClientStore::~AuthClientStore()
    {
        close();
    }

    void AuthClientStore::open(const std::filesystem::path& db_path)
    {
        std::lock_guard lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }

        path_ = db_path;
        const auto utf8 = db_path.string();
        const int open_result = sqlite3_open_v2(
            utf8.c_str(),
            &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr);
        if (open_result != SQLITE_OK) {
            const int code = open_result;
            const std::string message =
                std::string("sqlite3_open_v2(") + utf8 + ')';
            if (db_) {
                throwSqlite(db_, message, code);
            }
            throw std::runtime_error(
                message + " failed: SQLite " + std::to_string(code));
        }

        try {
            ensureSchema();
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    void AuthClientStore::close() noexcept
    {
        std::lock_guard lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool AuthClientStore::isOpen() const noexcept
    {
        std::lock_guard lock(mutex_);
        return db_ != nullptr;
    }

    int AuthClientStore::readUserVersion() const
    {
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql = "PRAGMA user_version;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare PRAGMA user_version", sqlite3_errcode(db_));
        }
        int version = 0;
        const int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            version = sqlite3_column_int(statement, 0);
        } else if (step != SQLITE_DONE) {
            sqlite3_finalize(statement);
            throwSqlite(db_, "PRAGMA user_version", sqlite3_errcode(db_));
        }
        sqlite3_finalize(statement);
        return version;
    }

    bool AuthClientStore::clientsTableExists() const
    {
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT 1 FROM sqlite_master "
            "WHERE type='table' AND name='clients' LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare clients table lookup", sqlite3_errcode(db_));
        }
        const int step = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (step == SQLITE_ROW) {
            return true;
        }
        if (step == SQLITE_DONE) {
            return false;
        }
        throwSqlite(db_, "clients table lookup", sqlite3_errcode(db_));
    }

    std::vector<std::string> AuthClientStore::readClientColumns() const
    {
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql = "PRAGMA table_info(clients);";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare PRAGMA table_info", sqlite3_errcode(db_));
        }

        std::vector<std::string> columns;
        while (true) {
            const int step = sqlite3_step(statement);
            if (step == SQLITE_ROW) {
                const auto* name = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, 1));
                columns.emplace_back(name ? name : "");
                continue;
            }
            if (step == SQLITE_DONE) {
                break;
            }
            sqlite3_finalize(statement);
            throwSqlite(db_, "PRAGMA table_info", sqlite3_errcode(db_));
        }
        sqlite3_finalize(statement);
        return columns;
    }

    void AuthClientStore::rejectIncompatibleSchema() const
    {
        throw std::runtime_error(kIncompatibleSchemaMessage);
    }

    void AuthClientStore::createSchemaV2()
    {
        execOrThrow(
            "CREATE TABLE clients ("
            "  client_id TEXT PRIMARY KEY NOT NULL,"
            "  client_name TEXT NOT NULL,"
            "  device_type TEXT NOT NULL,"
            "  device_id TEXT NOT NULL,"
            "  enabled INTEGER NOT NULL DEFAULT 1,"
            "  signature_meta TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL,"
            "  updated_at INTEGER NOT NULL"
            ");");
        execOrThrow(
            "CREATE INDEX IF NOT EXISTS idx_clients_lookup "
            "ON clients(client_id, client_name, device_type, device_id, enabled);");
        execOrThrow("PRAGMA user_version = 2;");
    }

    void AuthClientStore::ensureSchema()
    {
        execOrThrow("PRAGMA journal_mode=WAL;");

        const bool has_table = clientsTableExists();
        if (!has_table) {
            createSchemaV2();
            return;
        }

        const int user_version = readUserVersion();
        const auto columns = readClientColumns();
        const bool compatible =
            user_version == kAuthClientsSchemaUserVersion &&
            hasColumn(columns, "client_id") &&
            hasColumn(columns, "client_name") &&
            hasColumn(columns, "device_type") &&
            hasColumn(columns, "device_id") &&
            hasColumn(columns, "enabled") &&
            hasColumn(columns, "signature_meta") &&
            hasColumn(columns, "created_at") &&
            hasColumn(columns, "updated_at") &&
            !hasColumn(columns, "flash_serial");
        if (!compatible) {
            rejectIncompatibleSchema();
        }
        execOrThrow(
            "CREATE INDEX IF NOT EXISTS idx_clients_lookup "
            "ON clients(client_id, client_name, device_type, device_id, enabled);");
    }

    void AuthClientStore::execOrThrow(const char* sql) const
    {
        char* error = nullptr;
        const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        if (result != SQLITE_OK) {
            std::string detail = error ? error : "unknown";
            sqlite3_free(error);
            throw std::runtime_error(
                std::string("sqlite3_exec failed: ") + detail);
        }
    }

    std::int64_t AuthClientStore::nowUnix()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    void AuthClientStore::upsertClient(
        const std::string& client_id,
        const std::string& client_name,
        const std::string& device_type,
        const std::string& device_id,
        bool enabled,
        const std::string& signature_meta)
    {
        if (client_id.empty() || client_name.empty() || device_type.empty() ||
            device_id.empty())
        {
            throw std::invalid_argument(
                "client_id, client_name, device_type and device_id must be "
                "non-empty");
        }
        if (!IsSupportedDeviceType(device_type)) {
            throw std::invalid_argument(
                "device_type must be usb or computer");
        }

        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        const auto stamp = nowUnix();
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "INSERT INTO clients("
            "client_id, client_name, device_type, device_id, enabled, "
            "signature_meta, created_at, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
            "ON CONFLICT(client_id) DO UPDATE SET "
            "client_name=excluded.client_name, "
            "device_type=excluded.device_type, "
            "device_id=excluded.device_id, "
            "enabled=excluded.enabled, "
            "signature_meta=excluded.signature_meta, "
            "updated_at=excluded.updated_at;";

        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare upsertClient", sqlite3_errcode(db_));
        }

        sqlite3_bind_text(statement, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, client_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, device_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, device_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, enabled ? 1 : 0);
        sqlite3_bind_text(
            statement, 6, signature_meta.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 7, stamp);
        sqlite3_bind_int64(statement, 8, stamp);

        const int step = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (step != SQLITE_DONE) {
            throwSqlite(db_, "upsertClient", sqlite3_errcode(db_));
        }
    }

    std::optional<AuthClientRecord> AuthClientStore::getClient(
        const std::string& client_id) const
    {
        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT client_id, client_name, device_type, device_id, enabled, "
            "signature_meta, created_at, updated_at "
            "FROM clients WHERE client_id = ?1 LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare getClient", sqlite3_errcode(db_));
        }
        sqlite3_bind_text(statement, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<AuthClientRecord> record;
        const int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            record = readClientRow(statement);
        } else if (step != SQLITE_DONE) {
            sqlite3_finalize(statement);
            throwSqlite(db_, "getClient", sqlite3_errcode(db_));
        }
        sqlite3_finalize(statement);
        return record;
    }

    std::vector<AuthClientRecord> AuthClientStore::listClients() const
    {
        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT client_id, client_name, device_type, device_id, enabled, "
            "signature_meta, created_at, updated_at "
            "FROM clients ORDER BY client_id COLLATE NOCASE;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare listClients", sqlite3_errcode(db_));
        }

        std::vector<AuthClientRecord> rows;
        while (true) {
            const int step = sqlite3_step(statement);
            if (step == SQLITE_ROW) {
                rows.push_back(readClientRow(statement));
                continue;
            }
            if (step == SQLITE_DONE) {
                break;
            }
            sqlite3_finalize(statement);
            throwSqlite(db_, "listClients", sqlite3_errcode(db_));
        }
        sqlite3_finalize(statement);
        return rows;
    }

    void AuthClientStore::setEnabled(const std::string& client_id, bool enabled)
    {
        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "UPDATE clients SET enabled = ?1, updated_at = ?2 "
            "WHERE client_id = ?3;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare setEnabled", sqlite3_errcode(db_));
        }
        sqlite3_bind_int(statement, 1, enabled ? 1 : 0);
        sqlite3_bind_int64(statement, 2, nowUnix());
        sqlite3_bind_text(statement, 3, client_id.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(statement);
        const int changes = sqlite3_changes(db_);
        sqlite3_finalize(statement);
        if (step != SQLITE_DONE) {
            throwSqlite(db_, "setEnabled", sqlite3_errcode(db_));
        }
        if (changes == 0) {
            throw std::runtime_error("client_id not found: " + client_id);
        }
    }

    void AuthClientStore::removeClient(const std::string& client_id)
    {
        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql = "DELETE FROM clients WHERE client_id = ?1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare removeClient", sqlite3_errcode(db_));
        }
        sqlite3_bind_text(statement, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(statement);
        const int changes = sqlite3_changes(db_);
        sqlite3_finalize(statement);
        if (step != SQLITE_DONE) {
            throwSqlite(db_, "removeClient", sqlite3_errcode(db_));
        }
        if (changes == 0) {
            throw std::runtime_error("client_id not found: " + client_id);
        }
    }

    std::optional<AuthClientRecord> AuthClientStore::findEnabledMatch(
        const std::string& client_id,
        const std::string& client_name,
        const std::string& device_type,
        const std::string& device_id) const
    {
        std::lock_guard lock(mutex_);
        if (!db_) {
            throw std::runtime_error("AuthClientStore is not open");
        }

        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT client_id, client_name, device_type, device_id, enabled, "
            "signature_meta, created_at, updated_at "
            "FROM clients "
            "WHERE client_id = ?1 AND client_name = ?2 AND device_type = ?3 "
            "AND device_id = ?4 AND enabled = 1 "
            "LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throwSqlite(db_, "prepare findEnabledMatch", sqlite3_errcode(db_));
        }
        sqlite3_bind_text(statement, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, client_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, device_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, device_id.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<AuthClientRecord> record;
        const int step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            record = readClientRow(statement);
        } else if (step != SQLITE_DONE) {
            sqlite3_finalize(statement);
            throwSqlite(db_, "findEnabledMatch", sqlite3_errcode(db_));
        }
        sqlite3_finalize(statement);
        return record;
    }
}
