#include "SQLiteBackup.h"

#include "MyUtils/Encoding.h"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string pathToUtf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

std::string lowerExtension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return extension;
}

bool hasSQLiteHeader(const fs::path& path)
{
    constexpr std::array<char, 16> signature{
        'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
        'o', 'r', 'm', 'a', 't', ' ', '3', '\0'
    };

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    std::array<char, signature.size()> header{};
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    return
        stream.gcount() == static_cast<std::streamsize>(header.size()) &&
        header == signature;
}

std::string sqliteMessage(sqlite3* database, int code)
{
    std::ostringstream message;
    message
        << "SQLite error " << code
        << " (" << sqlite3_errstr(code) << ')';
    if (database != nullptr) {
        const char* detail = sqlite3_errmsg(database);
        if (detail != nullptr && std::strcmp(detail, "not an error") != 0) {
            message << ": " << detail;
        }
    }
    return message.str();
}

bool execute(sqlite3* database,
             const char* sql,
             std::string& error_message)
{
    char* sqlite_error = nullptr;
    const int result = sqlite3_exec(
        database,
        sql,
        nullptr,
        nullptr,
        &sqlite_error
    );
    if (result == SQLITE_OK) {
        return true;
    }

    error_message =
        sqlite_error != nullptr
            ? std::string(sqlite_error)
            : sqliteMessage(database, result);
    sqlite3_free(sqlite_error);
    return false;
}

bool verifyIntegrity(sqlite3* database, std::string& error_message)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        database,
        "PRAGMA integrity_check",
        -1,
        &statement,
        nullptr
    );
    if (prepare_result != SQLITE_OK) {
        error_message = sqliteMessage(database, prepare_result);
        return false;
    }

    bool saw_ok = false;
    bool valid = true;
    std::ostringstream details;
    for (;;) {
        const int step_result = sqlite3_step(statement);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            error_message = sqliteMessage(database, step_result);
            valid = false;
            break;
        }

        const unsigned char* text = sqlite3_column_text(statement, 0);
        const std::string line =
            text == nullptr
                ? std::string()
                : reinterpret_cast<const char*>(text);
        if (line == "ok") {
            saw_ok = true;
        } else {
            if (details.tellp() > 0) {
                details << "; ";
            }
            details << line;
            valid = false;
        }
    }
    sqlite3_finalize(statement);

    if (!valid) {
        if (error_message.empty()) {
            error_message =
                details.tellp() > 0
                    ? details.str()
                    : "integrity_check failed";
        }
        return false;
    }
    if (!saw_ok) {
        error_message = "integrity_check returned no result";
        return false;
    }
    return true;
}

bool queryText(sqlite3* database,
               const char* sql,
               std::string& value,
               std::string& error_message)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result =
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (prepare_result != SQLITE_OK) {
        error_message = sqliteMessage(database, prepare_result);
        return false;
    }

    const int step_result = sqlite3_step(statement);
    if (step_result != SQLITE_ROW) {
        error_message = sqliteMessage(database, step_result);
        sqlite3_finalize(statement);
        return false;
    }

    const unsigned char* text = sqlite3_column_text(statement, 0);
    value =
        text == nullptr
            ? std::string()
            : reinterpret_cast<const char*>(text);
    sqlite3_finalize(statement);
    return true;
}

std::uint64_t fnv1a64(const char* data, size_t size)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < size; ++index) {
        hash ^= static_cast<unsigned char>(data[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream stream;
    stream
        << std::hex
        << std::setfill('0')
        << std::setw(16)
        << value;
    return stream.str();
}

class DatabaseHandle {
public:
    ~DatabaseHandle()
    {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
    }

    sqlite3** output()
    {
        return &database_;
    }

    sqlite3* get() const
    {
        return database_;
    }

private:
    sqlite3* database_ = nullptr;
};

bool isRemoteSourcePath(const fs::path& path)
{
#ifdef _WIN32
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    if (error) {
        return false;
    }

    std::array<wchar_t, MAX_PATH + 1> volume_root{};
    if (!GetVolumePathNameW(
            absolute.c_str(),
            volume_root.data(),
            static_cast<DWORD>(volume_root.size())
        ))
    {
        const std::wstring value = absolute.wstring();
        return value.starts_with(L"\\\\");
    }
    return GetDriveTypeW(volume_root.data()) == DRIVE_REMOTE;
#else
    (void)path;
    return false;
#endif
}

} // namespace

bool isSQLiteSidecar(const fs::path& path)
{
    std::string name = path.filename().string();
    std::transform(
        name.begin(),
        name.end(),
        name.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return
        name.ends_with("-journal") ||
        name.ends_with("-wal") ||
        name.ends_with("-shm");
}

bool isSQLiteDatabaseCandidate(const fs::path& path)
{
    if (isSQLiteSidecar(path)) {
        return false;
    }

    std::string filename = path.filename().string();
    std::transform(
        filename.begin(),
        filename.end(),
        filename.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    // Windows Explorer's legacy thumbnail cache is an OLE compound file,
    // not SQLite. Treating every *.db as SQLite would reject otherwise valid
    // application-directory snapshots that happen to contain Thumbs.db.
    if (filename == "thumbs.db") {
        return false;
    }

    const std::string extension = lowerExtension(path);
    if (extension == ".db" ||
        extension == ".db3" ||
        extension == ".sqlite" ||
        extension == ".sqlite3")
    {
        return true;
    }
    return hasSQLiteHeader(path);
}

SQLiteSourceFingerprint inspectSQLiteSource(
    const fs::path& source,
    std::chrono::milliseconds busy_timeout)
{
    SQLiteSourceFingerprint result;
    if (isRemoteSourcePath(source)) {
        result.message =
            "network SQLite source is not allowed; run BackupService on "
            "the database host and configure a local filesystem path";
        return result;
    }

    DatabaseHandle database;
    const std::string source_utf8 = pathToUtf8(source);
    const int open_result = sqlite3_open_v2(
        source_utf8.c_str(),
        database.output(),
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (open_result != SQLITE_OK) {
        result.message = sqliteMessage(database.get(), open_result);
        return result;
    }
    sqlite3_extended_result_codes(database.get(), 1);
    sqlite3_busy_timeout(
        database.get(),
        static_cast<int>(busy_timeout.count())
    );

    std::string error_message;
    if (!execute(database.get(), "BEGIN", error_message)) {
        result.message = "cannot start source read transaction: " +
            error_message;
        return result;
    }

    std::string journal_mode;
    std::string schema_version;
    if (!queryText(
            database.get(),
            "PRAGMA journal_mode",
            journal_mode,
            error_message
        ) ||
        !queryText(
            database.get(),
            "PRAGMA schema_version",
            schema_version,
            error_message
        ))
    {
        result.message = "cannot inspect SQLite source: " + error_message;
        return result;
    }
    std::transform(
        journal_mode.begin(),
        journal_mode.end(),
        journal_mode.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    std::error_code filesystem_error;
    const std::uint64_t size = fs::file_size(source, filesystem_error);
    if (filesystem_error) {
        result.message =
            "cannot read SQLite source size: " + filesystem_error.message();
        return result;
    }
    const auto write_time = fs::last_write_time(source, filesystem_error);
    if (filesystem_error) {
        result.message =
            "cannot read SQLite source timestamp: " +
            filesystem_error.message();
        return result;
    }

    constexpr size_t sqlite_header_size = 100;
    std::array<char, sqlite_header_size> header{};
    size_t header_bytes = 0;
    if (size != 0) {
        std::ifstream stream(source, std::ios::binary);
        if (!stream.is_open()) {
            result.message = "cannot open SQLite source header";
            return result;
        }
        stream.read(
            header.data(),
            static_cast<std::streamsize>(header.size())
        );
        header_bytes = static_cast<size_t>(stream.gcount());
        if (header_bytes != sqlite_header_size) {
            result.message = "SQLite source has an incomplete header";
            return result;
        }
        constexpr std::array<char, 16> signature{
            'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
            'o', 'r', 'm', 'a', 't', ' ', '3', '\0'
        };
        if (!std::equal(signature.begin(), signature.end(), header.begin())) {
            result.message = "SQLite source has an invalid header";
            return result;
        }
    }

    if (!execute(database.get(), "COMMIT", error_message)) {
        result.message = "cannot finish source read transaction: " +
            error_message;
        return result;
    }

    const bool rollback_journal =
        journal_mode == "delete" ||
        journal_mode == "truncate" ||
        journal_mode == "persist";
    std::ostringstream fingerprint;
    fingerprint
        << "sqlite-v1:"
        << journal_mode
        << ':'
        << size
        << ':'
        << write_time.time_since_epoch().count()
        << ':'
        << hex64(fnv1a64(header.data(), header_bytes));

    result.ok = true;
    result.cacheable = rollback_journal;
    result.value = fingerprint.str();
    result.journal_mode = journal_mode;
    result.message =
        rollback_journal
            ? "stable rollback-journal fingerprint"
            : "journal mode is not safe for persistent cache reuse";
    return result;
}

SQLiteBackupResult backupSQLiteDatabase(
    const fs::path& source,
    const fs::path& destination,
    const SQLiteBackupOptions& options)
{
    SQLiteBackupResult result;
    if (isRemoteSourcePath(source)) {
        result.message =
            "network SQLite source is not allowed; run BackupService on "
            "the database host and configure a local filesystem path";
        return result;
    }

    const std::string source_utf8 = pathToUtf8(source);
    const std::string destination_utf8 = pathToUtf8(destination);

    DatabaseHandle source_database;
    int sqlite_result = sqlite3_open_v2(
        source_utf8.c_str(),
        source_database.output(),
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (sqlite_result != SQLITE_OK) {
        result.message = sqliteMessage(source_database.get(), sqlite_result);
        return result;
    }
    sqlite3_extended_result_codes(source_database.get(), 1);
    sqlite3_busy_timeout(
        source_database.get(),
        static_cast<int>(options.busy_timeout.count())
    );

    DatabaseHandle destination_database;
    sqlite_result = sqlite3_open_v2(
        destination_utf8.c_str(),
        destination_database.output(),
        SQLITE_OPEN_READWRITE |
            SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (sqlite_result != SQLITE_OK) {
        result.message =
            sqliteMessage(destination_database.get(), sqlite_result);
        return result;
    }
    sqlite3_extended_result_codes(destination_database.get(), 1);
    sqlite3_busy_timeout(
        destination_database.get(),
        static_cast<int>(options.busy_timeout.count())
    );

    std::string pragma_error;
    if (!execute(
            destination_database.get(),
            "PRAGMA journal_mode=DELETE;"
            "PRAGMA synchronous=FULL;",
            pragma_error
        ))
    {
        result.message = "cannot configure destination: " + pragma_error;
        return result;
    }

    sqlite3_backup* backup = sqlite3_backup_init(
        destination_database.get(),
        "main",
        source_database.get(),
        "main"
    );
    if (backup == nullptr) {
        result.message =
            sqliteMessage(
                destination_database.get(),
                sqlite3_errcode(destination_database.get())
            );
        return result;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + options.busy_timeout;
    int step_result = SQLITE_OK;
    for (;;) {
        // AutoPad databases are small. Copying all pages in one step holds a
        // shared source lock briefly and prevents a writer from changing the
        // database half-way through the snapshot.
        step_result = sqlite3_backup_step(backup, -1);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_BUSY && step_result != SQLITE_LOCKED) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(options.retry_delay);
    }

    const int finish_result = sqlite3_backup_finish(backup);
    if (step_result != SQLITE_DONE) {
        result.message =
            step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED
                ? "source remained busy until the backup timeout expired"
                : sqliteMessage(destination_database.get(), step_result);
        return result;
    }
    if (finish_result != SQLITE_OK) {
        result.message =
            sqliteMessage(destination_database.get(), finish_result);
        return result;
    }

    if (options.integrity_check) {
        std::string integrity_error;
        if (!verifyIntegrity(
                destination_database.get(),
                integrity_error
            ))
        {
            result.message =
                "snapshot integrity_check failed: " + integrity_error;
            return result;
        }
    }

    std::error_code size_error;
    result.size = fs::file_size(destination, size_error);
    if (size_error) {
        result.message =
            "cannot read snapshot size: " + size_error.message();
        return result;
    }

    result.ok = true;
    result.message = "SQLite online backup completed; integrity_check=ok";
    return result;
}

bool verifySQLiteDatabase(
    const fs::path& database,
    std::string& error_message,
    std::chrono::milliseconds busy_timeout)
{
    DatabaseHandle handle;
    const std::string database_utf8 = pathToUtf8(database);
    const int open_result = sqlite3_open_v2(
        database_utf8.c_str(),
        handle.output(),
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (open_result != SQLITE_OK) {
        error_message = sqliteMessage(handle.get(), open_result);
        return false;
    }
    sqlite3_extended_result_codes(handle.get(), 1);
    sqlite3_busy_timeout(
        handle.get(),
        static_cast<int>(busy_timeout.count())
    );
    return verifyIntegrity(handle.get(), error_message);
}

const char* backupSQLiteVersion() noexcept
{
    return sqlite3_libversion();
}
