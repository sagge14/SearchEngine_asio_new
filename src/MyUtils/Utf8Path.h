#pragma once

#include "MyUtils/Encoding.h"

#include <filesystem>
#include <string>

namespace encoding {

inline std::filesystem::path utf8_to_path(const std::string& utf8)
{
    return std::filesystem::path(utf8_to_wstring(utf8));
}

inline std::string path_to_utf8(const std::filesystem::path& path)
{
    return wstring_to_utf8(path.wstring());
}

inline std::string utf8_path_join(
    const std::string& directory,
    const std::string& name)
{
    return path_to_utf8(utf8_to_path(directory) / utf8_to_wstring(name));
}

} // namespace encoding
