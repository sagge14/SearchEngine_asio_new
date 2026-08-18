#pragma once
#include <string>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include "sqlite3.h"

class SQLiteOpenError : public std::runtime_error {
    std::string path_;
    int sqliteRc_{0};

public:
    SQLiteOpenError(std::string path, int sqliteRc, const std::string& sqliteMessage);
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] int sqliteRc() const noexcept;
};

class SQLiteQueryError : public std::runtime_error {
    std::string path_;
    std::string sqliteMessage_;
    int sqliteRc_{0};

public:
    SQLiteQueryError(std::string path, int sqliteRc, std::string sqliteMessage);
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] int sqliteRc() const noexcept;
    [[nodiscard]] const std::string& sqliteMessage() const noexcept;
    [[nodiscard]] bool isSchemaFailure() const noexcept;
};

class mySQLite {
    sqlite3 *db{nullptr};
    std::string dir;
    char *zErrMsg{nullptr};
    mutable std::mutex dbMutex, listMutex;
    std::list<std::map<std::string, std::string>> list;

    static int callback(void *data, int argc, char **argv, char **azColName);

public:
    enum class OpenMode {
        ReadWriteCreate,
        ReadOnly
    };

    explicit mySQLite(const std::string& base_dir);
    mySQLite(const std::string& base_dir, OpenMode mode);
    ~mySQLite();

    void execSql(const std::string& sql);
    void execSql(const std::string& dir, const std::string& sql);

    const std::map<std::string, std::string>& getFront() const;
    const std::map<std::string, std::string>& getBack() const;

    bool empty() const;
    size_t size() const;

    using Row              = std::map<std::string,std::string>;
    using RowList          = std::list<Row>;
    using const_iterator   = RowList::const_iterator;

    [[nodiscard]] RowList queryRows(const std::string& sql);

    /* --- «квери-подобные» новинки --- */
    void first();                 // курсор → первый ряд
    void next();                  // курсор++
    [[nodiscard]] bool eof() const;  // достигнут конец?
    [[nodiscard]] const Row& current() const; // текущий ряд

    /* STL-итераторы (для range-for) */
    [[nodiscard]] const_iterator begin() const;
    [[nodiscard]] const_iterator end() const;

    /* Быстрый доступ к полю текущего ряда */
    [[nodiscard]] const std::string& value(const std::string& field) const;

private:
    void connect(const std::string& base_dir);
    void close();
    [[noreturn]] void throwQueryError(int sqliteRc, const std::string& sqliteMessage);

    OpenMode openMode_{OpenMode::ReadWriteCreate};

    /* новый член-состояние курсора */
    const_iterator                cur_{list.cend()};
};
