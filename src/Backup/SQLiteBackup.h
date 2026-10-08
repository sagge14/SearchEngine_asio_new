#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

struct SQLiteBackupOptions {
    std::chrono::milliseconds busy_timeout{30000};
    std::chrono::milliseconds retry_delay{100};
    bool integrity_check = true;
};

struct SQLiteBackupResult {
    bool ok = false;
    std::uint64_t size = 0;
    std::string message;
};

struct SQLiteSourceFingerprint {
    bool ok = false;
    bool cacheable = false;
    std::string value;
    std::string journal_mode;
    std::string message;
};

bool isSQLiteDatabaseCandidate(const std::filesystem::path& path);
bool isSQLiteSidecar(const std::filesystem::path& path);

/// Reads a stable source signature while SQLite holds a read transaction.
///
/// Rollback-journal databases can be cached between service runs. WAL and
/// unusual journal modes deliberately return cacheable=false.
SQLiteSourceFingerprint inspectSQLiteSource(
    const std::filesystem::path& source,
    std::chrono::milliseconds busy_timeout = std::chrono::milliseconds(30000)
);

SQLiteBackupResult backupSQLiteDatabase(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const SQLiteBackupOptions& options = {}
);

bool verifySQLiteDatabase(
    const std::filesystem::path& database,
    std::string& error_message,
    std::chrono::milliseconds busy_timeout = std::chrono::milliseconds(30000)
);

const char* backupSQLiteVersion() noexcept;
