//
// Created by Sg on 12.10.2024.
//

#ifndef SEARCHENGINE_MD5HASHER_H
#define SEARCHENGINE_MD5HASHER_H
#include <iostream>
#include "SQLite/mySQLite.h"


class md5Hasher {

public:
    static std::string calculateMD5(const std::wstring& fileName);
};


#endif //SEARCHENGINE_MD5HASHER_H
