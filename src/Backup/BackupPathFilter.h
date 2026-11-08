#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/// Validates one user-supplied exclude pattern (config-time).
/// On success, *normalized_out receives the canonical pattern form used at
/// runtime (forward slashes, preserved root/directory markers).
bool validateExcludePattern(const std::string& pattern,
                            std::string& normalized_out,
                            std::string& error_message);

/// Small gitignore-like matcher for BackupService source trees.
/// Patterns must already be validated (e.g. via validateExcludePattern).
class BackupPathFilter {
public:
    BackupPathFilter() = default;
    explicit BackupPathFilter(std::vector<std::string> patterns);

    bool empty() const noexcept
    {
        return patterns_.empty();
    }

    const std::vector<std::string>& patterns() const noexcept
    {
        return patterns_;
    }

    /// True when a regular file at relative_path is excluded.
    bool isFileExcluded(const fs::path& relative_path) const;

    /// True when a directory at relative_path is excluded (and recursion may
    /// be pruned with disable_recursion_pending()).
    bool isDirectoryExcluded(const fs::path& relative_path) const;

private:
    struct Segment {
        bool double_star = false;
        std::wstring text;
    };

    struct CompiledPattern {
        std::string raw;
        bool directory_only = false;
        bool name_only = false;
        std::vector<Segment> segments;
    };

    static std::wstring normalizeForMatch(std::wstring value);
    static std::wstring pathToMatchString(const fs::path& relative_path);
    static std::vector<std::wstring> splitPathComponents(const std::wstring& path);
    static bool matchComponent(const std::wstring& pattern,
                               const std::wstring& value);
    static bool matchSegments(const std::vector<Segment>& pattern,
                              const std::vector<std::wstring>& path,
                              size_t pattern_index,
                              size_t path_index);
    static CompiledPattern compileValidated(const std::string& pattern);

    bool matches(const fs::path& relative_path, bool is_directory) const;

    std::vector<std::string> patterns_;
    std::vector<CompiledPattern> compiled_;
};
