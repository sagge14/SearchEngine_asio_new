#include "MyUtils/PathExclusion.h"

#include <gtest/gtest.h>

#include <clocale>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct ExclusionCase
{
    std::wstring excluded;
    std::wstring candidate;
    bool expected;
};

}  // namespace

TEST(PathExclusionTest, SubtreeSemantics)
{
    const std::vector<ExclusionCase> cases{
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\TEMP", true},
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\TEMP\\a.txt", true},
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\TEMP\\A\\b.txt", true},
        {L"D:\\DATA\\TEMP", L"d:\\data\\temp\\a.txt", true},
        {L"D:\\DATA\\TEMP", L"D:/DATA/TEMP/a.txt", true},
        {L"D:\\DATA\\TEMP\\", L"D:\\DATA\\TEMP\\a.txt", true},
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\TEMP_OLD\\a.txt", false},
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\MYTEMP\\a.txt", false},
        {L"D:\\DATA\\TEMP", L"D:\\DATA\\TEMP2\\a.txt", false},
    };

    const std::vector<std::string> excludedUtf8{"D:\\DATA\\TEMP"};
    for (const auto& testCase : cases) {
        EXPECT_EQ(
            path_exclusion::isPathExcluded(fs::path(testCase.candidate), excludedUtf8),
            testCase.expected)
            << "candidate=" << testCase.candidate.c_str();
    }
}

TEST(PathExclusionTest, UncCaseInsensitiveSubtree)
{
    const std::vector<std::string> excluded{
        "\\\\SERVER\\Share\\DATA\\TEMP"};
    EXPECT_TRUE(path_exclusion::isPathExcluded(
        fs::path(L"\\\\server\\share\\data\\temp\\a.txt"), excluded));
    EXPECT_FALSE(path_exclusion::isPathExcluded(
        fs::path(L"\\\\server\\share\\data\\temp_old\\a.txt"), excluded));
}

TEST(PathExclusionTest, CyrillicCaseInsensitiveIndependentOfLocale)
{
    const char* previous = setlocale(LC_ALL, nullptr);
    ASSERT_NE(setlocale(LC_ALL, "C"), nullptr);

    const std::vector<std::string> excluded{"D:\\ДАННЫЕ\\ТЕМП"};
    EXPECT_TRUE(path_exclusion::isPathExcluded(
        fs::path(L"D:\\данные\\темп\\a.txt"), excluded));
    EXPECT_FALSE(path_exclusion::isPathExcluded(
        fs::path(L"D:\\данные\\темп_old\\a.txt"), excluded));

    if (previous != nullptr) {
        setlocale(LC_ALL, previous);
    }
}

TEST(PathExclusionTest, EmptyExclusionListIsNoOp)
{
    EXPECT_FALSE(path_exclusion::isPathExcluded(
        fs::path(L"D:\\DATA\\TEMP\\a.txt"),
        {}));
}

TEST(PathExclusionTest, ExclusionOutsideRootsIsHarmlessNoOp)
{
    const std::vector<std::string> excludedUtf8{"E:\\OTHER\\TEMP"};
    EXPECT_FALSE(path_exclusion::isPathExcluded(
        fs::path(L"D:\\DATA\\keep\\a.txt"),
        excludedUtf8));
}
