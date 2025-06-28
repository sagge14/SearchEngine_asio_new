//
// Created by Sg on 11.10.2024.
//

#include "SaveDefaultCmd.h"
#include <codecvt>
#include <locale>

namespace cc
{
    std::string wstringToString(const std::wstring& wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

// Функция для перекодировки string в wstring
    std::wstring stringToWstring(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(str);
    }
}


std::filesystem::path SaveFileDefaultCmd::getBasePath() {

    return std::filesystem::path{SaveFileCmd::defaultSaveDirectory};

}

SaveFileDefaultCmd::SaveFileDefaultCmd(const std::string& _defaultSaveDirectory) {
    defaultSaveDirectory = std::filesystem::path{cc::stringToWstring(_defaultSaveDirectory)};
}