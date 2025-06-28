//
// Created by Sg on 02.08.2023.
//
#include "mySql.h"
#include <utility>

int mySql::callback(void *data, int argc, char **argv, char **azColName) {


    mySql::getInstance().list.emplace_back();
    auto back = &mySql::getInstance().list.back();
    for(int i = 0; i<argc; i++) {
        if(argv[i] == nullptr)
            back->insert(std::make_pair(azColName[i], ""));
        else
            back->insert(std::make_pair(azColName[i], argv[i]));

    }
    return 0;
}

void mySql::excSql(const std::string &sql) {

    const char *data = nullptr;

    int rc = sqlite3_exec(mySql::getInstance().db, sql.c_str(), callback, (void*)data, &mySql::getInstance().zErrMsg);

    if( rc != SQLITE_OK )
        sqlite3_free(mySql::getInstance().zErrMsg);

   // sqlite3_close(mySql::getInstance().db);
}

mySql &mySql::getInstance() {
    return getInstanceImpl();
}

void mySql::init(const std::string &_dir) {
    getInstanceImpl(_dir);
}

mySql &mySql::getInstanceImpl(const std::string &dir) {
    static mySql instance{ dir };
    return instance;
}

mySql::mySql(const std::string& _dir)  {

        if (!dir.empty() && db != nullptr)
            sqlite3_close(db);

        dir = _dir;

        if (dir.empty())
            throw std::runtime_error{ "mySql not initialized" };

        sqlite3_open(dir.c_str(), &db);
}

std::map<std::string,std::string> mySql::getFront() {
    return mySql::getInstance().list.front();
}

std::map<std::string,std::string> mySql::getBack() {
    return mySql::getInstance().list.back();
}

bool mySql::empty() {
    return mySql::getInstance().list.empty();
}

void mySql::excSql(const std::string &_dir, const std::string &sql) {

   std::lock_guard<std::mutex> my_lock(mt);

    mySql::getInstance().list.clear();
    sqlite3_close(mySql::getInstance().db);
    sqlite3_open(_dir.c_str(), &mySql::getInstance().db);
    excSql(sql);
    sqlite3_close(mySql::getInstance().db);
    sqlite3_open(mySql::getInstance().dir.c_str(), &mySql::getInstance().db);

}
