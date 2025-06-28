//
// Created by Sg on 02.08.2023.
//
#pragma once
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <stdexcept>
#include "sqlite3.h"

#define SQL_FRONT_VALUE(KEY) mySql::getFront().at(#KEY)
#define SQL_INST mySql::getInstance()
#define SQL_EMPTY mySql::getInstance().empty()

class mySql {

    sqlite3 *db{};
    std::string dir;
    char *zErrMsg = nullptr;
    static inline std::mutex mt{};
    std::list<std::map<std::string,std::string>> list;

    static int callback(void *data, int argc, char **argv, char **azColName);

    explicit mySql(const std::string&  _dir);
    static mySql& getInstanceImpl(const std::string& dir = "");

public:

    static mySql& getInstance();
    static void excSql(const std::string& dir, const std::string& sql);
    static void init(const std::string& _dir); // enable moving in
    static void excSql(const std::string& sql);
    static std::map<std::string,std::string> getFront();
    static std::map<std::string,std::string> getBack();
    static bool empty();
    static auto begin() {return getInstance().list.begin();};
    static auto end() {return getInstance().list.end();};
    static auto size() {return getInstance().list.size();};

};


