#include "SQLiteConnectionManager.h"


SQLiteConnectionManager& SQLiteConnectionManager::instance()
{
    static SQLiteConnectionManager mgr;
    return mgr;
}

SQLiteConnectionManager::SQLiteConnectionManager() = default;

std::shared_ptr<mySQLite>
SQLiteConnectionManager::getConnection(const std::string& dbPath)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(dbPath);
    if (it != connections_.end())
        return it->second;

    auto conn = std::make_shared<mySQLite>(dbPath);
    connections_[dbPath] = conn;
    return conn;
}

std::shared_ptr<mySQLite>
SQLiteConnectionManager::getReadOnlyConnection(const std::string& dbPath)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = readOnlyConnections_.find(dbPath);
    if (it != readOnlyConnections_.end())
        return it->second;

    auto conn = std::make_shared<mySQLite>(dbPath, mySQLite::OpenMode::ReadOnly);
    readOnlyConnections_[dbPath] = conn;
    return conn;
}

void SQLiteConnectionManager::closeConnection(const std::string& dbPath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(dbPath);
    readOnlyConnections_.erase(dbPath);
}
