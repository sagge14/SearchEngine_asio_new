#include "FileWatcher/FileEventFilter.h"
#include "MyUtils/FileExtensionContract.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using file_extension_contract::Selection;

TEST(FileExtensionContractTest, MatchesOnlyExactFinalExtension)
{
    const Selection txt{{"txt"}, false};

    EXPECT_TRUE(file_extension_contract::matchesPath(L"D:\\ROOT\\file.txt", txt));
    EXPECT_TRUE(file_extension_contract::matchesPath(L"D:\\ROOT\\FILE.TXT", txt));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"D:\\ROOT\\file.mytxt", txt));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"D:\\ROOT\\filetxt", txt));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"D:\\ROOT\\file.txxtxt", txt));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"D:\\ROOT\\file.xt", txt));
}

TEST(FileExtensionContractTest, UsesOnlyLastExtension)
{
    EXPECT_TRUE(file_extension_contract::matchesPath(
        L"archive.tar.gz", Selection{{"gz"}, false}));
    EXPECT_FALSE(file_extension_contract::matchesPath(
        L"archive.tar.gz", Selection{{"tar"}, false}));
}

TEST(FileExtensionContractTest, ExtensionlessContractIsExplicit)
{
    const Selection extensionless{{}, true};
    EXPECT_TRUE(file_extension_contract::matchesPath(L"README", extensionless));
    EXPECT_TRUE(file_extension_contract::matchesPath(L".hidden", extensionless));
    EXPECT_TRUE(file_extension_contract::matchesPath(L"name.", extensionless));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"name.txt", extensionless));

    const Selection none{{}, false};
    EXPECT_FALSE(file_extension_contract::matchesPath(L"README", none));
    EXPECT_FALSE(file_extension_contract::matchesPath(L"name.txt", none));
}

TEST(FileExtensionContractTest, UnicodeComparisonIsOrdinalCaseInsensitive)
{
    const Selection cyrillic{{"ТХТ"}, false};
    EXPECT_TRUE(file_extension_contract::matchesPath(L"файл.тхт", cyrillic));
}

TEST(FileExtensionContractTest, WatcherUsesTheProductionMatcher)
{
    const Selection txt{{"txt"}, false};
    EXPECT_TRUE(file_event_filter::matchesConfiguredExtension(
        L"D:\\ROOT\\file.txt", txt));
    EXPECT_FALSE(file_event_filter::matchesConfiguredExtension(
        L"D:\\ROOT\\file.mytxt", txt));
    EXPECT_FALSE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Added,
        L"D:\\ROOT\\file.mytxt",
        txt,
        {}));
}

TEST(FileExtensionContractTest, CanonicalValidationRejectsAmbiguousTokens)
{
    const std::vector<std::string> invalidTokens{
        "", ".txt", "tar.gz", "t*xt", "t?xt",
        "folder/txt", "folder\\txt", " txt", "txt "};
    for (const std::string& invalid : invalidTokens)
    {
        const auto errors =
            file_extension_contract::validateCanonicalSelection(
                Selection{{invalid}, false});
        EXPECT_FALSE(errors.empty()) << invalid;
    }

    EXPECT_FALSE(file_extension_contract::validateCanonicalSelection(
        Selection{{"txt", "TXT"}, false}).empty());
    EXPECT_TRUE(file_extension_contract::validateCanonicalSelection(
        Selection{{}, true}).empty());
    EXPECT_FALSE(file_extension_contract::validateCanonicalSelection(
        Selection{{}, false}).empty());
}

TEST(FileExtensionContractTest, LegacySelectionSplitsAndDeduplicates)
{
    const auto selection =
        file_extension_contract::canonicalizeLegacySelection(
            {"txt", "", "TXT", ".atl"});
    ASSERT_EQ(selection.indexedExtensions.size(), 2u);
    EXPECT_EQ(selection.indexedExtensions[0], "txt");
    EXPECT_EQ(selection.indexedExtensions[1], "atl");
    EXPECT_TRUE(selection.includeExtensionlessFiles);

    const bool explicitFalse = false;
    EXPECT_FALSE(file_extension_contract::canonicalizeLegacySelection(
        {"txt", ""}, &explicitFalse).includeExtensionlessFiles);
}

}  // namespace
