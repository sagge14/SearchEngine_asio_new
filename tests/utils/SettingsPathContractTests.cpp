#include "MyUtils/SettingsPathContract.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(SettingsPathContractTest, AcceptsLocalAndUncRoots)
{
    const std::vector<std::string> roots{"D:\\DATA", "\\\\server\\share\\DATA"};
    const auto result = settings_path_contract::validateConfiguredIndexPaths(roots, {});
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(SettingsPathContractTest, AcceptsUncExclusions)
{
    const std::vector<std::string> roots{"D:\\DATA"};
    const std::vector<std::string> excluded{"\\\\server\\share\\DATA\\TEMP"};
    const auto result =
        settings_path_contract::validateConfiguredIndexPaths(roots, excluded);
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(SettingsPathContractTest, RejectsRelativeIndexRoots)
{
    const auto result = settings_path_contract::validateConfiguredIndexPaths(
        {"relative\\data"}, {});
    EXPECT_FALSE(result.ok);
}

TEST(SettingsPathContractTest, RejectsRelativeExcludedSubtrees)
{
    const auto result = settings_path_contract::validateConfiguredIndexPaths(
        {"D:\\DATA"}, {"relative\\temp"});
    EXPECT_FALSE(result.ok);
}

TEST(SettingsPathContractTest, RejectsEmptyExcludedSubtreeString)
{
    const auto result = settings_path_contract::validateConfiguredIndexPaths(
        {"D:\\DATA"}, {""});
    EXPECT_FALSE(result.ok);
}

TEST(SettingsPathContractTest, MissingPhysicalRootRemainsSyntacticallyValid)
{
    const auto result = settings_path_contract::validateConfiguredIndexPaths(
        {"D:\\nonexistent-se-index-root-for-test"}, {});
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(SettingsPathContractTest, EmptyIndexRootsIsInvalid)
{
    const auto result = settings_path_contract::validateConfiguredIndexPaths({}, {});
    EXPECT_FALSE(result.ok);
}
