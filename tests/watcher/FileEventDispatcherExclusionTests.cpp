#include "MyUtils/PathExclusion.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool wouldQueueForIndexing(
    const std::wstring& path,
    const std::vector<std::string>& excludedSubtrees)
{
    return !path_exclusion::isPathExcluded(fs::path(path), excludedSubtrees);
}

}  // namespace

TEST(FileEventDispatcherExclusionTest, ExcludedLivePathIsFilteredAtDispatcherBoundary)
{
    const std::vector<std::string> excluded{"D:\\ROOT\\excluded"};
    EXPECT_FALSE(wouldQueueForIndexing(L"D:\\ROOT\\excluded\\new.txt", excluded));
    EXPECT_TRUE(wouldQueueForIndexing(L"D:\\ROOT\\excluded_similar\\new.txt", excluded));
}

TEST(FileEventDispatcherExclusionTest, ForceWalkUsesSamePredicateAsLiveEvents)
{
    const std::vector<std::string> excluded{"D:\\ROOT\\excluded"};
    EXPECT_FALSE(wouldQueueForIndexing(L"D:\\ROOT\\excluded\\nested\\seed.txt", excluded));
    EXPECT_TRUE(wouldQueueForIndexing(L"D:\\ROOT\\keep\\seed.txt", excluded));
}
