#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "mySQLite.h"

class SQLiteConnectionManager {
public:
    static SQLiteConnectionManager& instance();

    std::shared_ptr<mySQLite> getConnection(const std::string& dbPath);
    void closeConnection(const std::string& dbPath);

private:
    SQLiteConnectionManager();
    ~SQLiteConnectionManager() = default;

    SQLiteConnectionManager(const SQLiteConnectionManager&) = delete;
    SQLiteConnectionManager& operator=(const SQLiteConnectionManager&) = delete;

    std::map<std::string, std::shared_ptr<mySQLite>> connections_;
    std::mutex mutex_;
};
