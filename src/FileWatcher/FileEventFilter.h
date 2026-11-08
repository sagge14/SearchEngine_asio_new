#pragma once

#include "FileWatcher/FileEvent.h"
#include "MyUtils/FileExtensionContract.h"
#include "MyUtils/PathExclusion.h"

#include <filesystem>
#include <string>
#include <vector>

namespace file_event_filter {

inline bool matchesConfiguredExtension(
    const std::wstring& path,
    const file_extension_contract::Selection& fileTypes)
{
    return file_extension_contract::matchesPath(path, fileTypes);
}

/// Production live-event predicate used by FileEventDispatcher::pushFileEvent.
/// Removal/RenamedOld stay accepted so stale index records can be cleaned up.
inline bool shouldAcceptFileEvent(
    FileEvent evt,
    const std::wstring& path,
    const file_extension_contract::Selection& fileTypes,
    const std::vector<std::string>& excludedSubtrees)
{
    if (!matchesConfiguredExtension(path, fileTypes)) {
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
