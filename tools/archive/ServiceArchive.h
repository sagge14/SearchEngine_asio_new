#pragma once

#include "ArchiveCore.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace searchengine_archive {

struct ServiceInvocation {
    std::wstring imagePath;
    fs::path executable;
    fs::path dataDirectory;
    std::wstring serviceNameArgument;
};

struct InstalledService {
    std::wstring serviceName;
    std::wstring displayName;
    std::wstring imagePath;
    fs::path executable;
    fs::path dataDirectory;
    std::uint32_t currentState{};
};

struct ServiceArchiveOptions {
    std::wstring serviceName;
    fs::path archiveRoot;
};

struct ServiceArchivePlan {
    ServiceArchiveOptions options;
    InstalledService service;
    int year{};
    fs::path finalDirectory;
    fs::path originalInstallDirectory;
    fs::path archivedExecutable;
    fs::path archivedDataDirectory;
    std::wstring archivedImagePath;
    fs::path originalPrmMonthlyDirectory;
    fs::path originalPrdMonthlyDirectory;
    std::vector<PathMapping> mappings;
    std::vector<MonthlyDatabase> monthlyDatabases;
    std::vector<std::string> warnings;
};

struct ServiceArchiveResult {
    bool ok{false};
    fs::path archiveDirectory;
    fs::path manifestPath;
    std::string message;
};

enum class ServiceRestoreMode {
    OriginalLocations,
    SelectedRoot
};

/// Pure, SCM-independent restore layout. Archive paths are rebased under the
/// recorded original locations or under an operator-selected root.
struct ServiceRestorePlan {
    ServiceRestoreMode mode{ServiceRestoreMode::SelectedRoot};
    fs::path archiveDirectory;
    fs::path restoreRoot;
    bool sourceCleanupCompleted{false};
    int year{};
    std::wstring serviceName;
    std::wstring archivedImagePath;
    fs::path restoredExecutable;
    fs::path restoredDataDirectory;
    std::wstring restoredImagePath;
    fs::path restoredPrmMonthlyDirectory;
    fs::path restoredPrdMonthlyDirectory;
    /// Directory trees: archived source -> final destination.
    std::vector<PathMapping> mappings;
    /// Individual monthly SQLite files: archived source -> final destination.
    std::vector<PathMapping> monthlyDatabases;
    /// Every destination directory that must be absent before copying starts.
    std::vector<fs::path> requiredDirectories;
};

[[nodiscard]] ServiceInvocation parseServiceInvocation(
    const std::wstring& imagePath);

[[nodiscard]] std::wstring buildServiceImagePath(
    const fs::path& executable,
    const std::wstring& serviceName,
    const fs::path& dataDirectory);

[[nodiscard]] std::vector<InstalledService> enumerateSearchEngineServices();

[[nodiscard]] InstalledService inspectSearchEngineService(
    const std::wstring& serviceName);

[[nodiscard]] ServiceArchivePlan planServiceArchive(
    const ServiceArchiveOptions& options);

void rewriteDocumentCatalogPaths(
    const fs::path& database,
    const std::vector<PathMapping>& mappings);

void rewriteSettingsForArchive(
    const fs::path& settingsPath,
    const std::vector<PathMapping>& mappings,
    const fs::path& prmMonthlyDirectory,
    const fs::path& prdMonthlyDirectory);

void rewriteSettingsForActive(
    const fs::path& settingsPath,
    const std::vector<PathMapping>& archiveToOriginalMappings,
    const fs::path& prmMonthlyDirectory,
    const fs::path& prdMonthlyDirectory);

void mergeRestoreStagingTree(
    const fs::path& staging,
    const fs::path& target);

/// Resolves the operator's restore destination without Win32 drive-relative
/// ambiguity. A bare drive designator (for example D:) means D:\.
[[nodiscard]] fs::path normalizeServiceRestoreRoot(
    const fs::path& restoreRoot);

/// Returns the complete managed installation directory for a service
/// executable. The packaged layout is <install>\bin\SearchEngine.exe; legacy
/// layouts with the executable directly in the managed directory remain
/// supported.
[[nodiscard]] fs::path serviceInstallDirectory(
    const fs::path& executable);

/// Performs the no-reparse-point/no-special-file preflight for complete
/// removal of managed runtime directories. All roots are checked before any
/// deletion is attempted.
void validateServiceRuntimeCleanupDirectories(
    const std::vector<fs::path>& roots);

/// Removes complete, already archive-verified runtime directory trees. The
/// function repeats the full preflight before the first deletion.
void removeServiceRuntimeCleanupDirectories(
    const std::vector<fs::path>& roots,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceRestorePlan planServiceRestore(
    const fs::path& archiveDirectory,
    const fs::path& restoreRoot);

[[nodiscard]] ServiceRestorePlan planServiceRestoreOriginalLocations(
    const fs::path& archiveDirectory);

[[nodiscard]] ServiceArchiveResult executeServiceArchive(
    const ServiceArchivePlan& plan,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult restoreServiceArchive(
    const fs::path& archiveDirectory,
    const fs::path& restoreRoot,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult restoreServiceArchiveOriginalLocations(
    const fs::path& archiveDirectory,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult validateRestoredServiceArchiveDeletion(
    const fs::path& archiveDirectory,
    const std::vector<InstalledService>& installedServices);

[[nodiscard]] ServiceArchiveResult deleteRestoredServiceArchive(
    const fs::path& archiveDirectory,
    const std::vector<InstalledService>& installedServices,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult deleteRestoredServiceArchive(
    const fs::path& archiveDirectory,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult cleanupServiceArchiveSources(
    const fs::path& archiveDirectory,
    bool deleteMonthlyDatabases,
    const ProgressCallback& progress = {});

} // namespace searchengine_archive
