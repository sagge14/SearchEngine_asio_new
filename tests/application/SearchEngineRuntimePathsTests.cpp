#include "Application/SearchEngineOptions.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    SearchEngineRuntimePaths resolveForDataDir(const fs::path& dataDir)
    {
        SearchEngineOptions options;
        options.data_dir = dataDir;
        const fs::path executable =
            fs::path(L"C:") / L"Program Files" / L"SearchEngineService" /
            L"SearchEngine.exe";
        return resolveSearchEngineRuntimePaths(options, executable);
    }
}

TEST(SearchEngineRuntimePaths, PrefixMapIsUnderResolvedDataDir)
{
    const fs::path dataDir =
        fs::path(L"C:") / L"ProgramData" / L"SearchEngineService";
    const auto paths = resolveForDataDir(dataDir);

    EXPECT_EQ(paths.prefix_map, paths.data_dir / L"prefix_map.json");
    EXPECT_EQ(paths.prefix_map.filename(), fs::path(L"prefix_map.json"));
}

TEST(SearchEngineRuntimePaths, DistinctInstanceDataDirsYieldDistinctPrefixMaps)
{
    const auto defaultPaths = resolveForDataDir(
        fs::path(L"C:") / L"ProgramData" / L"SearchEngineService");
    const auto instancePaths = resolveForDataDir(
        fs::path(L"C:") / L"ProgramData" / L"SearchEngineService-year2026");

    EXPECT_EQ(
        defaultPaths.prefix_map,
        defaultPaths.data_dir / L"prefix_map.json");
    EXPECT_EQ(
        instancePaths.prefix_map,
        instancePaths.data_dir / L"prefix_map.json");
    EXPECT_NE(defaultPaths.prefix_map, instancePaths.prefix_map);
    EXPECT_NE(defaultPaths.data_dir, instancePaths.data_dir);
}

TEST(SearchEngineOptions, InitialUpdateIsASeparateNonInteractiveMode)
{
    SearchEngineOptions options;
    std::string error;
    const std::vector<std::wstring> arguments{
        L"SearchEngine.exe",
        L"--initial-update",
        L"--data-dir",
        L"C:\\ProgramData\\SearchEngineService-2026"
    };

    ASSERT_TRUE(parseSearchEngineOptions(arguments, options, error)) << error;
    EXPECT_EQ(options.mode, SearchEngineLaunchMode::InitialUpdate);
    EXPECT_EQ(
        options.data_dir,
        fs::path(L"C:\\ProgramData\\SearchEngineService-2026"));
}

TEST(SearchEngineOptions, InitialUpdateRejectsServiceMode)
{
    SearchEngineOptions options;
    std::string error;
    const std::vector<std::wstring> arguments{
        L"SearchEngine.exe",
        L"--service",
        L"--initial-update"
    };

    EXPECT_FALSE(parseSearchEngineOptions(arguments, options, error));
    EXPECT_NE(error.find("mutually exclusive"), std::string::npos);
}

TEST(SearchEngineOptions, InitialUpdateRequiresExplicitDataDir)
{
    SearchEngineOptions options;
    std::string error;
    const std::vector<std::wstring> arguments{
        L"SearchEngine.exe",
        L"--initial-update"
    };

    EXPECT_FALSE(parseSearchEngineOptions(arguments, options, error));
    EXPECT_NE(error.find("requires an explicit --data-dir"), std::string::npos);
}
