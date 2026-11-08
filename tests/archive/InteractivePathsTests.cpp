#include "InteractivePaths.h"

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using searchengine_archive::DirectoryInputAction;

TEST(ArchiveInteractivePaths, EmptyInputOpensFolderPickerWithSuggestedDirectory)
{
    const fs::path suggested = L"D:\\archive";
    const fs::path selected = L"E:\\selected";
    int calls = 0;
    const auto result = searchengine_archive::resolveDirectoryInput(
        L"", suggested, false,
        [&](const fs::path& initial) -> std::optional<fs::path> {
            ++calls;
            EXPECT_EQ(initial, suggested);
            return selected;
        });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result.action, DirectoryInputAction::Selected);
    EXPECT_EQ(result.path, selected);
}

TEST(ArchiveInteractivePaths, DashDisablesOnlyOptionalDirectory)
{
    const auto optional = searchengine_archive::resolveDirectoryInput(
        L"-", L"D:\\BASES", true,
        [](const fs::path&) { return std::optional<fs::path>{}; });
    EXPECT_EQ(optional.action, DirectoryInputAction::Disabled);
    EXPECT_TRUE(optional.path.empty());
    EXPECT_THROW(
        (void)searchengine_archive::resolveDirectoryInput(
            L"-", L"D:\\archive", false,
            [](const fs::path&) { return std::optional<fs::path>{}; }),
        std::invalid_argument);
}

TEST(ArchiveInteractivePaths, PickerCancellationNeverProducesEmptySelection)
{
    const auto result = searchengine_archive::resolveDirectoryInput(
        L"", {}, false,
        [](const fs::path&) { return std::optional<fs::path>{}; });
    EXPECT_EQ(result.action, DirectoryInputAction::PickerCancelled);
    EXPECT_TRUE(result.path.empty());
}

TEST(ArchiveInteractivePaths, ManualPathDoesNotOpenPicker)
{
    bool called = false;
    const auto result = searchengine_archive::resolveDirectoryInput(
        L"D:\\manual", L"E:\\suggested", false,
        [&](const fs::path&) -> std::optional<fs::path> {
            called = true;
            return L"E:\\wrong";
        });
    EXPECT_FALSE(called);
    EXPECT_EQ(result.action, DirectoryInputAction::Selected);
    EXPECT_EQ(result.path, fs::path(L"D:\\manual"));
}

TEST(ArchiveInteractivePaths, BareDriveDesignatorMeansDriveRoot)
{
    bool called = false;
    const auto result = searchengine_archive::resolveDirectoryInput(
        L"D:", {}, false,
        [&](const fs::path&) -> std::optional<fs::path> {
            called = true;
            return L"E:\\wrong";
        });
    EXPECT_FALSE(called);
    EXPECT_EQ(result.action, DirectoryInputAction::Selected);
    EXPECT_EQ(result.path, fs::path(L"D:\\"));
}

TEST(ArchiveInteractivePaths, AssumeYesNeverDeletesRestoredArchive)
{
    EXPECT_FALSE(searchengine_archive::shouldDeleteRestoredArchive(true, true));
    EXPECT_FALSE(searchengine_archive::shouldDeleteRestoredArchive(true, false));
    EXPECT_FALSE(searchengine_archive::shouldDeleteRestoredArchive(false, false));
    EXPECT_TRUE(searchengine_archive::shouldDeleteRestoredArchive(false, true));
}

} // namespace
