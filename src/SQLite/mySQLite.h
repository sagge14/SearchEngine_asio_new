#pragma once
#include <string>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include "sqlite3.h"

class mySQLite {
    sqlite3 *db{nullptr};
    std::string dir;
    char *zErrMsg{nullptr};
    mutable std::mutex dbMutex, listMutex;
    std::list<std::map<std::string, std::string>> list;

    static int callback(void *data, int argc, char **argv, char **azColName);

public:
    explicit mySQLite(const std::string& base_dir);
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

    /* новый член-состояние курсора */
    const_iterator                cur_{list.cend()};
};
