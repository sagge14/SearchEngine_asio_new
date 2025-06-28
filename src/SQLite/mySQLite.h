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

private:
    void connect(const std::string& base_dir);
    void close();
};
