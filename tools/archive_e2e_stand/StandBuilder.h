#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace searchengine_archive_e2e {

namespace fs = std::filesystem;

struct StandOptions {
    fs::path root;
    fs::path settingsTemplate;
    int year{2026};
    int recordsPerMonth{10};
};

struct ServiceArchiveStandOptions {
    StandOptions stand;
    /// Absolute location at which the generated directory will be unpacked on
    /// the disposable target machine. Paths in archive-operation.json and in
    /// the frozen Settings.json are written against this root.
    fs::path deploymentRoot;
    /// Complete portable app directory containing SearchEngine.exe and its
    /// runtime DLLs. The directory is copied into server/program.
    fs::path programTemplate;
    /// Windows 7 x86 build of this helper. It is copied into the stand and
    /// performs the first-run path preparation before service registration.
    fs::path preparerTemplate;
    /// Optional complete portable package. When supplied, it is bundled under
    /// installer/ and the stand gets an additional clean-VM deployment mode.
    /// The existing portable archive activation mode remains unchanged.
    fs::path installerTemplate;
    /// Original active location used by the legacy restore workflow.
    fs::path restoreRoot;
    std::wstring serviceName;
    int port{25027};
};

struct WorkstationStandOptions {
    fs::path root;
    /// Selected data volume root (for example D:\). Disposable tests may pass
    /// a dedicated empty directory instead of a real volume root.
    fs::path dataVolumeRoot;
    fs::path programFilesRoot;
    fs::path programDataRoot;
};

struct WorkstationStandLayout {
    fs::path standRoot;
    fs::path dataVolumeRoot;
    fs::path installRoot;
    fs::path binDirectory;
    fs::path toolsDirectory;
    fs::path dataDirectory;
    fs::path prmBaseDirectory;
    fs::path prdBaseDirectory;
    fs::path prmMonthlyDirectory;
    fs::path prdMonthlyDirectory;
    fs::path tlgDirectory;
    fs::path opisDirectory;
    fs::path raznDirectory;
    fs::path f12Directory;
    std::vector<fs::path> monthDirectories;
    std::wstring serviceName;
    int port{};
    int year{};
};

struct StandSummary {
    fs::path root;
    int year{};
    int databaseCount{};
    int telegramRowCount{};
    int uniqueTelegramCount{};
    int attachmentCount{};
    int f12WayRowCount{};
    std::uintmax_t generatedBytes{};
};

[[nodiscard]] StandSummary generateStand(const StandOptions& options);

[[nodiscard]] StandSummary generateServiceArchiveStand(
    const ServiceArchiveStandOptions& options);

[[nodiscard]] StandSummary prepareServiceArchiveStand(const fs::path& root);

/// Builds the production-like target paths and performs a complete collision
/// preflight without writing to the destination roots.
[[nodiscard]] WorkstationStandLayout planWorkstationStandDeployment(
    const WorkstationStandOptions& options);

/// Deploys only a generated synthetic stand. Windows service registration is
/// deliberately left to the generated administrator BAT after file checks.
[[nodiscard]] WorkstationStandLayout deployWorkstationStand(
    const WorkstationStandOptions& options);

[[nodiscard]] StandSummary verifyStand(const fs::path& root);

} // namespace searchengine_archive_e2e
