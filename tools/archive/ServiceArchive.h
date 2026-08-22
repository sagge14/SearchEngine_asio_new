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

[[nodiscard]] ServiceArchiveResult executeServiceArchive(
    const ServiceArchivePlan& plan,
    const ProgressCallback& progress = {});

[[nodiscard]] ServiceArchiveResult restoreServiceArchive(
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
