#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace path_exclusion {

std::filesystem::path normalizeComparablePath(const std::filesystem::path& path);

bool isPathInsideExcludedSubtree(
    const std::filesystem::path& candidate,
    const std::filesystem::path& excludedRoot);

bool isPathExcluded(
    const std::filesystem::path& candidate,
    const std::vector<std::string>& excludedSubtreesUtf8);

}  // namespace path_exclusion
