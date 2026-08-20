#include "FileWatcher/FileEventFilter.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

const std::vector<std::string> kTxt{"txt"};
const std::vector<std::string> kExcluded{"D:\\ROOT\\excluded"};

}  // namespace

TEST(FileEventDispatcherExclusionTest, ExcludedLivePathIsFilteredAtDispatcherBoundary)
{
    EXPECT_FALSE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Added, L"D:\\ROOT\\excluded\\new.txt", kTxt, kExcluded));
    EXPECT_FALSE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Modified, L"D:\\ROOT\\excluded\\new.txt", kTxt, kExcluded));
    EXPECT_FALSE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::RenamedNew, L"D:\\ROOT\\excluded\\new.txt", kTxt, kExcluded));
    EXPECT_TRUE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Added, L"D:\\ROOT\\excluded_similar\\new.txt", kTxt, kExcluded));
}

TEST(FileEventDispatcherExclusionTest, RemovalAndRenamedOldStayAcceptedForCleanup)
{
    EXPECT_TRUE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Removed, L"D:\\ROOT\\excluded\\gone.txt", kTxt, kExcluded));
    EXPECT_TRUE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::RenamedOld, L"D:\\ROOT\\excluded\\old.txt", kTxt, kExcluded));
}

TEST(FileEventDispatcherExclusionTest, ForceWalkUsesSamePredicateAsLiveEvents)
{
    EXPECT_FALSE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Added,
        L"D:\\ROOT\\excluded\\nested\\seed.txt",
        kTxt,
        kExcluded));
    EXPECT_TRUE(file_event_filter::shouldAcceptFileEvent(
        FileEvent::Added, L"D:\\ROOT\\keep\\seed.txt", kTxt, kExcluded));
}
