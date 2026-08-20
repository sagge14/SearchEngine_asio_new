#include "PathExclusion.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace path_exclusion {
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
void foldWindowsCase(std::wstring& value)
{
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
}

bool componentsEqual(wchar_t lhs, wchar_t rhs)
{
    wchar_t a = lhs;
    wchar_t b = rhs;
    CharLowerBuffW(&a, 1);
    CharLowerBuffW(&b, 1);
    return a == b;
}
#else
bool componentsEqual(wchar_t lhs, wchar_t rhs)
{
    return lhs == rhs;
}
#endif

std::wstring toComparableWstring(const fs::path& path)
{
    std::wstring value = path.lexically_normal().generic_wstring();
#ifdef _WIN32
    foldWindowsCase(value);
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
