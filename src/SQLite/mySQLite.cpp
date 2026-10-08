#include "mySQLite.h"

#include <utility>

namespace
{
    std::string makeOpenErrorMessage(
        const std::string& path,
        int sqliteRc,
        const std::string& sqliteMessage)
    {
        return "Failed to open database: " + path + ": " + sqliteMessage +
            " (" + std::to_string(sqliteRc) + ")";
    }

    std::string makeQueryErrorMessage(
        const std::string& path,
        int sqliteRc,
        const std::string& sqliteMessage)
    {
        return "SQL execution failed: " + sqliteMessage +
            " (path=" + path + ", rc=" + std::to_string(sqliteRc) + ")";
    }

    bool messageLooksLikeSchemaFailure(const std::string& message)
    {
        return message.find("no such table") != std::string::npos ||
            message.find("no such column") != std::string::npos;
    }
}

SQLiteOpenError::SQLiteOpenError(
    std::string path,
    int sqliteRc,
    const std::string& sqliteMessage)
    : std::runtime_error(makeOpenErrorMessage(path, sqliteRc, sqliteMessage))
    , path_(std::move(path))
    , sqliteRc_(sqliteRc)
{
}

const std::string& SQLiteOpenError::path() const noexcept
{
    return path_;
}

int SQLiteOpenError::sqliteRc() const noexcept
{
    return sqliteRc_;
}

SQLiteQueryError::SQLiteQueryError(
    std::string path,
    int sqliteRc,
    std::string sqliteMessage)
    : std::runtime_error(makeQueryErrorMessage(path, sqliteRc, sqliteMessage))
    , path_(std::move(path))
    , sqliteMessage_(std::move(sqliteMessage))
    , sqliteRc_(sqliteRc)
{
}

const std::string& SQLiteQueryError::path() const noexcept
{
    return path_;
}

int SQLiteQueryError::sqliteRc() const noexcept
{
    return sqliteRc_;
}

const std::string& SQLiteQueryError::sqliteMessage() const noexcept
{
    return sqliteMessage_;
}

bool SQLiteQueryError::isSchemaFailure() const noexcept
{
    switch (sqliteRc_) {
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
        case SQLITE_SCHEMA:
        case SQLITE_FORMAT:
            return true;
        default:
            return messageLooksLikeSchemaFailure(sqliteMessage_);
    }
}

int mySQLite::callback(void *data, int argc, char **argv, char **azColName) {
    auto *self = static_cast<mySQLite*>(data);
    std::lock_guard<std::mutex> lock(self->listMutex);

    self->list.emplace_back();
    auto &back = self->list.back();
    for (int i = 0; i < argc; i++) {
        back[azColName[i]] = argv[i] ? argv[i] : "";
    }
    return 0;
}

mySQLite::mySQLite(const std::string& base_dir)
    : openMode_(OpenMode::ReadWriteCreate)
{
    connect(base_dir);
}

mySQLite::mySQLite(const std::string& base_dir, OpenMode mode)
    : openMode_(mode)
{
    connect(base_dir);
}

mySQLite::~mySQLite() {
    close();
}

void mySQLite::connect(const std::string& base_dir) {
    std::lock_guard<std::mutex> lock(dbMutex);
    dir = base_dir;

    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }

    int rc = SQLITE_OK;
    if (openMode_ == OpenMode::ReadOnly) {
        rc = sqlite3_open_v2(dir.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    }
    else {
        rc = sqlite3_open(dir.c_str(), &db);
    }

    if (rc != SQLITE_OK) {
        const char* errmsg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        const std::string message = errmsg ? errmsg : "unknown SQLite error";
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw SQLiteOpenError(dir, rc, message);
    }
    sqlite3_busy_timeout(db, 3000);
    if (openMode_ != OpenMode::ReadOnly) {
        sqlite3_exec(db, "PRAGMA journal_mode=OFF;", nullptr, nullptr, nullptr);
    }
}

void mySQLite::close() {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

[[noreturn]] void mySQLite::throwQueryError(int sqliteRc, const std::string& sqliteMessage)
{
    throw SQLiteQueryError(dir, sqliteRc, sqliteMessage);
}

void mySQLite::execSql(const std::string& _dir, const std::string& _sql) {
    std::lock_guard<std::mutex> lock(dbMutex);
    close();
    connect(_dir);
    execSql(_sql);
    close();
    connect(this->dir); // Reconnect to the original directory
}

const std::map<std::string, std::string>& mySQLite::getFront() const {
    std::lock_guard<std::mutex> lock(listMutex);
    return list.front();
}

const std::map<std::string, std::string>& mySQLite::getBack() const {
    std::lock_guard<std::mutex> lock(listMutex);
    return list.back();
}

bool mySQLite::empty() const {
    std::lock_guard<std::mutex> lock(listMutex);
    return list.empty();
}

size_t mySQLite::size() const {
    std::lock_guard<std::mutex> lock(listMutex);
    return list.size();
}

void mySQLite::execSql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(dbMutex);
    {
        std::lock_guard<std::mutex> listLock(listMutex);
        list.clear();
    }

    int rc = sqlite3_exec(db, sql.c_str(), callback, this, &zErrMsg);
    if (rc != SQLITE_OK) {
        const std::string errMsg = zErrMsg ? zErrMsg : "unknown SQLite error";
        sqlite3_free(zErrMsg);
        zErrMsg = nullptr;
        throwQueryError(rc, errMsg);
    }

    /* курсор ставим на начало свежего результата */
    std::lock_guard<std::mutex> l(listMutex);
    cur_ = list.begin();
}

mySQLite::RowList mySQLite::queryRows(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(dbMutex);
    {
        std::lock_guard<std::mutex> listLock(listMutex);
        list.clear();
        cur_ = list.end();
    }

    const int rc = sqlite3_exec(db, sql.c_str(), callback, this, &zErrMsg);
    if (rc != SQLITE_OK) {
        const std::string errMsg = zErrMsg ? zErrMsg : "unknown SQLite error";
        sqlite3_free(zErrMsg);
        zErrMsg = nullptr;
        throwQueryError(rc, errMsg);
    }

    std::lock_guard<std::mutex> listLock(listMutex);
    cur_ = list.begin();
    return list;
}

void mySQLite::first()
{
    std::lock_guard<std::mutex> l(listMutex);
    cur_ = list.begin();
}

void mySQLite::next()
{
    std::lock_guard<std::mutex> l(listMutex);
    if (cur_ != list.end()) ++cur_;
}

bool mySQLite::eof() const
{
    std::lock_guard<std::mutex> l(listMutex);
    return cur_ == list.end();
}

const mySQLite::Row& mySQLite::current() const
{
    std::lock_guard<std::mutex> l(listMutex);
    if (cur_ == list.end())
        throw std::out_of_range("cursor is at end");
    return *cur_;
}

const std::string& mySQLite::value(const std::string& field) const
{
    return current().at(field);   // если нет поля — std::out_of_range
}

/* итераторы только-чтение, потокобезопасность на совести вызывающего */
mySQLite::const_iterator mySQLite::begin() const { return list.cbegin(); }
mySQLite::const_iterator mySQLite::end()   const { return list.cend(); }
