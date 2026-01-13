#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "mySQLite.h"

class SQLiteConnectionManager {
public:
    // Получить singleton instance
    static SQLiteConnectionManager& instance() {
        static SQLiteConnectionManager mgr;
        return mgr;
    }

    // Получить shared_ptr к соединению по пути к файлу
    std::shared_ptr<mySQLite> getConnection(const std::string& dbPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Если есть — вернуть
        auto it = connections_.find(dbPath);
        if (it != connections_.end())
            return it->second;
        // Если нет — создать и вернуть
        auto conn = std::make_shared<mySQLite>(dbPath);
        connections_[dbPath] = conn;
        return conn;
    }

    // По желанию — функция закрытия конкретного подключения
    void closeConnection(const std::string& dbPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(dbPath);
    }

private:
    std::map<std::string, std::shared_ptr<mySQLite>> connections_;
    std::mutex mutex_;
    SQLiteConnectionManager() = default;
    SQLiteConnectionManager(const SQLiteConnectionManager&) = delete;
    SQLiteConnectionManager& operator=(const SQLiteConnectionManager&) = delete;
};
