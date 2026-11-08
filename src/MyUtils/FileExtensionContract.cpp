#include "FileExtensionContract.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace file_extension_contract {
namespace {

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        throw std::invalid_argument("extension is too long");
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        throw std::invalid_argument("extension is not valid UTF-8");
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) != required)
    {
        throw std::invalid_argument("extension is not valid UTF-8");
    }
    return result;
}

bool equalOrdinalIgnoreCase(
    const std::wstring& left,
    const std::wstring& right) noexcept
{
    if (left.size() > static_cast<std::size_t>(INT_MAX) ||
        right.size() > static_cast<std::size_t>(INT_MAX))
    {
        return false;
    }
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

std::string validateCanonicalToken(const std::string& token)
{
    if (token.empty()) {
        return "must not contain empty strings";
    }

    std::wstring wide;
    try {
        wide = utf8ToWide(token);
    } catch (const std::invalid_argument&) {
        return "must contain valid UTF-8 strings";
    }

    if (wide.empty()) {
        return "must not contain empty strings";
    }
    if (std::iswspace(wide.front()) || std::iswspace(wide.back())) {
        return "must not contain leading or trailing whitespace";
    }
    if (wide.find_first_of(L"./\\*?") != std::wstring::npos) {
        return "must not contain dots, wildcards, or path separators";
    }
    return {};
}

std::wstring finalExtension(const std::wstring& path)
{
    std::wstring extension =
        std::filesystem::path(path).filename().extension().wstring();
    if (extension == L".") {
        return {};
    }
    if (!extension.empty() && extension.front() == L'.') {
        extension.erase(extension.begin());
    }
    return extension;
}

}  // namespace

std::vector<std::string> validateCanonicalSelection(
    const Selection& selection)
{
    std::vector<std::string> errors;
    std::vector<std::wstring> seen;
    seen.reserve(selection.indexedExtensions.size());

    for (const std::string& token : selection.indexedExtensions) {
        if (const std::string error = validateCanonicalToken(token);
            !error.empty())
        {
            errors.push_back(error);
            continue;
        }

        const std::wstring wide = utf8ToWide(token);
        if (std::any_of(
                seen.begin(), seen.end(),
                [&](const std::wstring& existing) {
                    return equalOrdinalIgnoreCase(existing, wide);
                }))
        {
            errors.emplace_back(
                "must not contain case-insensitive duplicate extensions");
            continue;
        }
        seen.push_back(wide);
    }

    if (selection.indexedExtensions.empty() &&
        !selection.includeExtensionlessFiles)
    {
        errors.emplace_back(
            "must select at least one extension or extensionless files");
    }
    return errors;
}

Selection canonicalizeLegacySelection(
    const std::vector<std::string>& legacyExtensions,
    const bool* explicitIncludeExtensionless)
{
    Selection result;
    bool legacyIncludesExtensionless = false;
    std::vector<std::wstring> seen;

    for (std::string token : legacyExtensions) {
        if (token.empty()) {
            legacyIncludesExtensionless = true;
            continue;
        }
        if (token.front() == '.') {
            token.erase(token.begin());
        }
        if (token.empty()) {
            legacyIncludesExtensionless = true;
            continue;
        }

        if (const std::string error = validateCanonicalToken(token);
            !error.empty())
        {
            throw std::invalid_argument(
                "legacy config.extensions " + error);
        }

        const std::wstring wide = utf8ToWide(token);
        if (std::any_of(
                seen.begin(), seen.end(),
                [&](const std::wstring& existing) {
                    return equalOrdinalIgnoreCase(existing, wide);
                }))
        {
            continue;
        }
        seen.push_back(wide);
        result.indexedExtensions.push_back(std::move(token));
    }

    result.includeExtensionlessFiles = explicitIncludeExtensionless
        ? *explicitIncludeExtensionless
        : legacyIncludesExtensionless;

    if (const auto errors = validateCanonicalSelection(result); !errors.empty()) {
        throw std::invalid_argument(
            "legacy config.extensions " + errors.front());
    }
    return result;
}

bool matchesPath(
    const std::wstring& path,
    const Selection& selection)
{
    if (selection.indexedExtensions.empty() &&
        !selection.includeExtensionlessFiles)
    {
        return false;
    }

    const std::wstring extension = finalExtension(path);
    if (extension.empty()) {
        return selection.includeExtensionlessFiles;
    }

    for (const std::string& configured : selection.indexedExtensions) {
        try {
            if (equalOrdinalIgnoreCase(extension, utf8ToWide(configured))) {
                return true;
            }
        } catch (const std::invalid_argument&) {
            return false;
        }
    }
    return false;
}

}  // namespace file_extension_contract
