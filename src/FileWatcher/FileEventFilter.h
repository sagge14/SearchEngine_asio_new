#pragma once

#include "FileWatcher/FileEvent.h"
#include "MyUtils/PathExclusion.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace file_event_filter {

inline bool matchesConfiguredExtension(
    const std::wstring& path,
    const std::vector<std::string>& extensions)
{
    using namespace std::filesystem;

    /* Если список пуст -- фильтра нет → разрешаем всё */
    if (extensions.empty())
        return true;

    std::wstring file = std::filesystem::path(path).filename().wstring();

    auto pos = file.rfind(L'.');
    bool hasDot   = pos != std::wstring::npos;
    bool dotAtEnd = hasDot && pos == file.size() - 1;
    bool noExt    = !hasDot || dotAtEnd;

    for (const auto& e8 : extensions)
    {
        std::wstring e (e8.begin(), e8.end());

        if (e.empty())
        {
            if (noExt)
                return true;
            continue;
        }

        if (!noExt && e.size() <= file.size() - pos - 1 &&
            std::equal(e.begin(), e.end(),
                       file.end() - e.size(),
                       [](wchar_t a, wchar_t b){
                           return std::towlower(a) == std::towlower(b);
                       }))
            return true;
    }
    return false;
}

/// Production live-event predicate used by FileEventDispatcher::pushFileEvent.
/// Removal/RenamedOld stay accepted so stale index records can be cleaned up.
inline bool shouldAcceptFileEvent(
    FileEvent evt,
    const std::wstring& path,
    const std::vector<std::string>& extensions,
    const std::vector<std::string>& excludedSubtrees)
{
    if (!matchesConfiguredExtension(path, extensions)) {
        return false;
    }
    if (evt != FileEvent::Removed && evt != FileEvent::RenamedOld &&
        path_exclusion::isPathExcluded(
            std::filesystem::path(path), excludedSubtrees))
    {
        return false;
    }
    return true;
}

}  // namespace file_event_filter
