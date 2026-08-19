#include "PathExclusion.h"

#include <cwctype>
#include <algorithm>

namespace path_exclusion {
namespace fs = std::filesystem;

namespace {

bool componentsEqual(wchar_t lhs, wchar_t rhs)
{
#ifdef _WIN32
    return std::towlower(lhs) == std::towlower(rhs);
#else
    return lhs == rhs;
#endif
}

std::wstring toComparableWstring(const fs::path& path)
{
    std::wstring value = path.lexically_normal().generic_wstring();
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
#endif
    while (value.size() > 1 &&
           (value.back() == L'/' || value.back() == L'\\'))
    {
        value.pop_back();
    }
    return value;
}

}  // namespace

fs::path normalizeComparablePath(const fs::path& path)
{
    return fs::path(toComparableWstring(path));
}

bool isPathInsideExcludedSubtree(
    const fs::path& candidate,
    const fs::path& excludedRoot)
{
    const std::wstring normalizedCandidate = toComparableWstring(candidate);
    const std::wstring normalizedExcluded = toComparableWstring(excludedRoot);
    if (normalizedCandidate.empty() || normalizedExcluded.empty()) {
        return false;
    }
    if (normalizedCandidate == normalizedExcluded) {
        return true;
    }
    if (normalizedCandidate.size() <= normalizedExcluded.size()) {
        return false;
    }
    if (!std::equal(
            normalizedExcluded.begin(),
            normalizedExcluded.end(),
            normalizedCandidate.begin(),
            componentsEqual))
    {
        return false;
    }
    return normalizedCandidate[normalizedExcluded.size()] == L'/';
}

bool isPathExcluded(
    const fs::path& candidate,
    const std::vector<std::string>& excludedSubtreesUtf8)
{
    for (const auto& excludedUtf8 : excludedSubtreesUtf8) {
        if (excludedUtf8.empty()) {
            continue;
        }
        if (isPathInsideExcludedSubtree(candidate, fs::u8path(excludedUtf8))) {
            return true;
        }
    }
    return false;
}

}  // namespace path_exclusion
