//
// Created by Sg on 29.10.2024.
//

#include "PrefixMap.h"

std::filesystem::path PrefixMap::getPath(const std::wstring &key) const {

        auto it = map_.find(key);
        if (it != map_.end()) {
            return std::filesystem::path(prefix + it->second);
        } else {
            throw std::runtime_error("error");
        }

}
