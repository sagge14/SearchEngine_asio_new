#pragma once

#include "MyUtils/WindowsPath.h"

#include <string>
#include <vector>

namespace settings_path_contract {

inline bool isValidIndexRoot(const std::string& value) noexcept
{
    return !value.empty() &&
        windows_path::isAbsoluteWindowsFilesystemPath(value);
}

inline bool isValidExcludedSubtree(const std::string& value) noexcept
{
    return !value.empty() &&
        windows_path::isAbsoluteWindowsFilesystemPath(value);
}

struct PathContractResult {
    bool ok{true};
    std::string error;
};

/// Syntax-only check used by direct server startup. Does not call exists()
/// or canonical(): a configured root may appear later (late-root watcher).
inline PathContractResult validateConfiguredIndexPaths(
    const std::vector<std::string>& indexRoots,
    const std::vector<std::string>& excludedSubtrees)
{
    if (indexRoots.empty()) {
        return {false, "config.index_roots is empty"};
    }
    for (const auto& root : indexRoots) {
        if (!isValidIndexRoot(root)) {
            return {
                false,
                "config.index_roots must contain absolute Windows paths"
            };
        }
    }
    for (const auto& path : excludedSubtrees) {
        if (!isValidExcludedSubtree(path)) {
            return {
                false,
                "config.excluded_subtrees must contain non-empty absolute "
                "Windows paths"
            };
        }
    }
    return {};
}

}  // namespace settings_path_contract
