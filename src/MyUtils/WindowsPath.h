#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace windows_path {

inline bool isPathSeparator(char ch) noexcept
{
    return ch == '\\' || ch == '/';
}

/// Drive-letter absolute path: `D:\DATA` or `D:/DATA`.
/// Rejects UNC and relative paths. Used for local-only production roots.
inline bool isAbsoluteWindowsLocalPath(std::string_view value) noexcept
{
    if (value.size() < 3) {
        return false;
    }
    if (isPathSeparator(value[0])) {
        return false;
    }
    const auto drive = static_cast<unsigned char>(value[0]);
    if (!std::isalpha(drive)) {
        return false;
    }
    if (value[1] != ':') {
        return false;
    }
    return isPathSeparator(value[2]);
}

/// UNC absolute path: `\\server\share` or `//server/share` plus optional rest.
inline bool isUncWindowsPath(std::string_view value) noexcept
{
    if (value.size() < 5) {
        return false;
    }
    if (!isPathSeparator(value[0]) || !isPathSeparator(value[1])) {
        return false;
    }
    std::size_t i = 2;
    if (isPathSeparator(value[i])) {
        return false;
    }
    while (i < value.size() && !isPathSeparator(value[i])) {
        ++i;
    }
    if (i >= value.size() || !isPathSeparator(value[i])) {
        return false;
    }
    ++i;
    if (i >= value.size() || isPathSeparator(value[i])) {
        return false;
    }
    return true;
}

/// Absolute Windows filesystem path: local drive or UNC.
/// Does not check that the path exists.
inline bool isAbsoluteWindowsFilesystemPath(std::string_view value) noexcept
{
    return isAbsoluteWindowsLocalPath(value) || isUncWindowsPath(value);
}

}  // namespace windows_path
