//
// Created by Sg on 01.10.2023.
//
#pragma once
#include <string>
#include <codecvt>
#include <filesystem>
#include <list>

class Interface {
    Interface() = default;
public:
    static Interface& getInstance()
    {
        static Interface instance;
        return instance;
    }

    /*
    std::list<int> list1 = { 5,9,0,1,3,4 };
    std::list<int> list2 = { 8,7,2,6,4 };
    list1.sort();
    list2.sort();
    std::cout << "list1:  " << list1 << "\n";
    std::cout << "list2:  " << list2 << "\n";
    list1.merge(list2);
    list1.unique();
    std::cout << "merged: " << list1 << "\n";
    */


    static std::list<std::wstring> getAllFilesFromDir(const std::string& dir, const std::list<std::string>& ext);
    static std::list<std::wstring> getAllFilesFromDirs(const std::list<std::string> dirs, const std::list<std::string> ext = std::list<std::string>{},
                                                       const std::vector<std::string> &excludeDirs = {});

    static std::list<std::wstring> getAllFilesFromDir2(const std::string &dir, const std::list<std::string> &exts,
                                                       const std::vector<std::string> &excludeDirs = {});
};
