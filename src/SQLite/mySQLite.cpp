#include "mySQLite.h"

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

mySQLite::mySQLite(const std::string& base_dir) {
    connect(base_dir);
}

mySQLite::~mySQLite() {
    close();
}

void mySQLite::connect(const std::string& base_dir) {
    std::lock_guard<std::mutex> lock(dbMutex);
    dir = base_dir;

    if (db) close();

    int rc = sqlite3_open(dir.c_str(), &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Failed to open database: " + dir);
    }
}

void mySQLite::close() {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

void mySQLite::execSql(const std::string& sql) {
    std::lock_guard<std::mutex> lock(dbMutex);
    list.clear();

    int rc = sqlite3_exec(db, sql.c_str(), callback, this, &zErrMsg);
    if (rc != SQLITE_OK) {
        std::string errMsg = zErrMsg;
        sqlite3_free(zErrMsg);
        throw std::runtime_error("SQL execution failed: " + errMsg);
    }
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
