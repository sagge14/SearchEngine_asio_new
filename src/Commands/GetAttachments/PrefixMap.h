//
// Created by Sg on 29.10.2024.
//

#ifndef SEARCHENGINE_PREFIXMAP_H
#define SEARCHENGINE_PREFIXMAP_H
#include <map>
#include <string>
#include <filesystem>

struct PrefixMap {
    std::wstring prefix;
    std::map<std::wstring, std::wstring> map_;
public:
    // Метод, возвращающий путь, составленный из prefix и значения из map_
    [[nodiscard]] std::filesystem::path getPath(const std::wstring& key) const;
};


#endif //SEARCHENGINE_PREFIXMAP_H
