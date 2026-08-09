#include "BackupPathFilter.h"

#include "MyUtils/Encoding.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <utility>

namespace {

std::string trimCopy(std::string value)
{
    const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    value.erase(
        value.begin(),
        std::find_if_not(value.begin(), value.end(), is_space)
    );
    value.erase(
        std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
        value.end()
    );
    return value;
}

std::string replaceBackslashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool hasDriveOrScheme(const std::string& pattern)
{
    return pattern.find(':') != std::string::npos;
}

bool isUncPattern(const std::string& pattern)
{
    return pattern.size() >= 2 && pattern[0] == '/' && pattern[1] == '/';
}

std::vector<std::string> splitUtf8Components(const std::string& pattern)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : pattern) {
        if (ch == '/') {
            parts.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    parts.push_back(std::move(current));
    return parts;
}

} // namespace

bool validateExcludePattern(const std::string& pattern,
                            std::string& normalized_out,
                            std::string& error_message)
{
    const std::string trimmed = trimCopy(pattern);
    if (trimmed.empty()) {
        error_message = "must not be empty";
        return false;
    }
    if (!trimmed.empty() && trimmed.front() == '!') {
        error_message =
            "negative patterns (!pattern) are not supported";
        return false;
    }
    if (hasDriveOrScheme(trimmed)) {
        error_message =
            "must be a relative pattern (absolute paths are not allowed)";
        return false;
    }

    std::string normalized = replaceBackslashes(trimmed);
    if (isUncPattern(normalized)) {
        error_message = "UNC paths are not allowed";
        return false;
    }

    bool directory_only = false;
    while (!normalized.empty() && normalized.back() == '/') {
        directory_only = true;
        normalized.pop_back();
    }
    bool rooted = false;
    if (!normalized.empty() && normalized.front() == '/') {
        rooted = true;
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }

    if (normalized.empty()) {
        error_message = "must not be empty";
        return false;
    }

    const std::vector<std::string> components =
        splitUtf8Components(normalized);
    for (const std::string& component : components) {
        if (component.empty()) {
            error_message = "must not contain empty path components";
            return false;
        }
        if (component == "." || component == "..") {
            error_message =
                "must not contain \".\" or \"..\" path components";
            return false;
        }
        if (component.find('\\') != std::string::npos) {
            error_message = "must use '/' as the path separator";
            return false;
        }
    }

    normalized_out.clear();
    if (rooted) {
        normalized_out.push_back('/');
    }
    normalized_out += normalized;
    if (directory_only) {
        normalized_out.push_back('/');
    }
    return true;
}

std::wstring BackupPathFilter::normalizeForMatch(std::wstring value)
{
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); }
    );
#endif
    return value;
}

std::wstring BackupPathFilter::pathToMatchString(const fs::path& relative_path)
{
    std::wstring value = relative_path.lexically_normal().generic_wstring();
    while (!value.empty() && (value.back() == L'/' || value.back() == L'\\')) {
        value.pop_back();
    }
    while (!value.empty() && (value.front() == L'/' || value.front() == L'\\')) {
        value.erase(value.begin());
    }
    std::replace(value.begin(), value.end(), L'\\', L'/');
    return normalizeForMatch(std::move(value));
}

std::vector<std::wstring> BackupPathFilter::splitPathComponents(
    const std::wstring& path)
{
    std::vector<std::wstring> parts;
    if (path.empty()) {
        return parts;
    }
    std::wstring current;
    for (wchar_t ch : path) {
        if (ch == L'/') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

bool BackupPathFilter::matchComponent(const std::wstring& pattern,
                                      const std::wstring& value)
{
    size_t pi = 0;
    size_t vi = 0;
    size_t star_pi = std::wstring::npos;
    size_t star_vi = std::wstring::npos;

    while (vi < value.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == L'?' || pattern[pi] == value[vi]))
        {
            ++pi;
            ++vi;
            continue;
        }
        if (pi < pattern.size() && pattern[pi] == L'*') {
            star_pi = pi++;
            star_vi = vi;
            continue;
        }
        if (star_pi != std::wstring::npos) {
            pi = star_pi + 1;
            vi = ++star_vi;
            continue;
        }
        return false;
    }

    while (pi < pattern.size() && pattern[pi] == L'*') {
        ++pi;
    }
    return pi == pattern.size();
}

bool BackupPathFilter::matchSegments(const std::vector<Segment>& pattern,
                                     const std::vector<std::wstring>& path,
                                     size_t pattern_index,
                                     size_t path_index)
{
    while (pattern_index < pattern.size()) {
        if (pattern[pattern_index].double_star) {
            if (pattern_index + 1 == pattern.size()) {
                return true;
            }
            for (size_t next = path_index; next <= path.size(); ++next) {
                if (matchSegments(pattern, path, pattern_index + 1, next)) {
                    return true;
                }
            }
            return false;
        }
        if (path_index >= path.size()) {
            return false;
        }
        if (!matchComponent(pattern[pattern_index].text, path[path_index])) {
            return false;
        }
        ++pattern_index;
        ++path_index;
    }
    return path_index == path.size();
}

BackupPathFilter::CompiledPattern BackupPathFilter::compileValidated(
    const std::string& pattern)
{
    CompiledPattern compiled;
    compiled.raw = pattern;

    std::string body = pattern;
    if (!body.empty() && body.back() == '/') {
        compiled.directory_only = true;
        body.pop_back();
    }
    bool rooted = false;
    if (!body.empty() && body.front() == '/') {
        rooted = true;
        body.erase(body.begin());
    }

    const std::wstring wide = normalizeForMatch(encoding::utf8_to_wstring(body));
    std::wstring current;
    auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        if (current == L"**") {
            compiled.segments.push_back(Segment{true, {}});
        } else {
            compiled.segments.push_back(Segment{false, current});
        }
        current.clear();
    };

    for (wchar_t ch : wide) {
        if (ch == L'/') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();

    compiled.name_only =
        !rooted &&
        compiled.segments.size() == 1 &&
        !compiled.segments.front().double_star;
    return compiled;
}

BackupPathFilter::BackupPathFilter(std::vector<std::string> patterns)
    : patterns_(std::move(patterns))
{
    compiled_.reserve(patterns_.size());
    for (const std::string& pattern : patterns_) {
        compiled_.push_back(compileValidated(pattern));
    }
}

bool BackupPathFilter::matches(const fs::path& relative_path,
                               bool is_directory) const
{
    if (compiled_.empty()) {
        return false;
    }

    const std::wstring path = pathToMatchString(relative_path);
    if (path.empty()) {
        return false;
    }
    const std::vector<std::wstring> components = splitPathComponents(path);
    if (components.empty()) {
        return false;
    }

    for (const CompiledPattern& pattern : compiled_) {
        if (pattern.directory_only && !is_directory) {
            continue;
        }
        if (pattern.name_only) {
            if (pattern.segments.size() != 1 ||
                pattern.segments.front().double_star)
            {
                continue;
            }
            if (matchComponent(pattern.segments.front().text, components.back())) {
                return true;
            }
            continue;
        }
        if (matchSegments(pattern.segments, components, 0, 0)) {
            return true;
        }
    }
    return false;
}

bool BackupPathFilter::isFileExcluded(const fs::path& relative_path) const
{
    return matches(relative_path, false);
}

bool BackupPathFilter::isDirectoryExcluded(const fs::path& relative_path) const
{
    return matches(relative_path, true);
}
