#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace searchengine_archive {

namespace fs = std::filesystem;

struct PathMapping {
    fs::path source;
    fs::path target;
};

struct MonthlyDatabase {
    enum class Kind { Prm, Prd };

    Kind kind{Kind::Prm};
    fs::path source;
    fs::path relativeTarget;
    int month{};
};

struct YearMoveOptions {
    int year{};
    fs::path prmMonthlyDirectory;
    fs::path prdMonthlyDirectory;
    fs::path archiveRoot;
};

struct YearMovePlan {
    YearMoveOptions options;
    fs::path finalDirectory;
    std::vector<MonthlyDatabase> databases;
    std::vector<PathMapping> mappings;
    std::vector<std::string> warnings;
};

struct YearMoveResult {
    bool ok{false};
    fs::path finalDirectory;
    fs::path manifestPath;
    std::uint64_t copiedFiles{};
    std::uint64_t copiedBytes{};
    std::string message;
};

using ProgressCallback = std::function<void(const std::wstring&)>;

[[nodiscard]] bool isPathEqualOrBelow(
    const fs::path& candidate,
    const fs::path& root);

[[nodiscard]] fs::path rebasePath(
    const fs::path& source,
    const std::vector<PathMapping>& mappings);

[[nodiscard]] bool isTlgArchiveRoot(const fs::path& path);

[[nodiscard]] bool shouldSkipTlgTopLevelEntry(
    const fs::path& topLevelName,
    bool isDirectory,
    int archivedYear);

[[nodiscard]] bool shouldSkipArchiveTreeEntry(
    const fs::path& sourceRoot,
    const fs::path& entryName,
    int depth,
    bool isDirectory,
    int archivedYear);

[[nodiscard]] std::vector<fs::path> collapseSourceRoots(
    std::vector<fs::path> roots);

[[nodiscard]] YearMovePlan planYearMove(const YearMoveOptions& options);

/// Shared inspection/rewrite primitives used by the standalone year move and
/// by the Windows-service archive transaction.
[[nodiscard]] std::vector<MonthlyDatabase> inspectMonthlyDatabases(
    const YearMoveOptions& options,
    std::vector<std::string>* warnings = nullptr);

[[nodiscard]] std::vector<fs::path> inspectAutoPadDirectToRoots(
    const fs::path& database);

void rewriteAutoPadDirectTo(
    const fs::path& database,
    const std::vector<PathMapping>& mappings);

[[nodiscard]] YearMoveResult executeYearMove(
    const YearMovePlan& plan,
    const ProgressCallback& progress = {});

[[nodiscard]] YearMoveResult cleanupYearMoveFiles(
    const fs::path& finalDirectory,
    bool deleteMonthlyDatabases,
    const ProgressCallback& progress = {});

} // namespace searchengine_archive
