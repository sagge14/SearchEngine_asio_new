#include <gtest/gtest.h>

#include "Index/DeferredPathQueue.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

TEST(DeferredPathQueue, CoalescesRepeatedPaths)
{
    inverted_index::DeferredPathQueue queue;
    EXPECT_TRUE(queue.defer(L"same.txt"));
    EXPECT_FALSE(queue.defer(L"same.txt"));
    EXPECT_FALSE(queue.defer(L"same.txt"));
    EXPECT_EQ(queue.size(), 1u);
}

TEST(DeferredPathQueue, ConcurrentDeferDuringDrainIsNotLost)
{
    inverted_index::DeferredPathQueue queue;
    queue.defer(L"first.txt");

    std::promise<void> replayEntered;
    std::promise<void> releaseReplay;
    std::shared_future<void> mayFinish = releaseReplay.get_future().share();

    std::thread drain([&] {
        queue.drainOnce([&](const std::wstring&) {
            replayEntered.set_value();
            mayFinish.wait();
        });
    });

    replayEntered.get_future().wait();
    EXPECT_TRUE(queue.defer(L"second.txt"));
    releaseReplay.set_value();
    drain.join();

    const auto remaining = queue.takeAll();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front(), L"second.txt");
}

TEST(DeferredPathQueue, UpdateThenDeleteReplaysActualAbsence)
{
    inverted_index::DeferredPathQueue queue;
    const fs::path path = fs::temp_directory_path() / "deferred_deleted.txt";
    { std::ofstream file(path); file << "content"; }
    queue.defer(path.wstring());
    std::error_code error;
    fs::remove(path, error);

    bool replayedAsDeletion = false;
    queue.drainOnce([&](const std::wstring& item) {
        replayedAsDeletion = !fs::is_regular_file(item);
    });
    EXPECT_TRUE(replayedAsDeletion);
}

TEST(DeferredPathQueue, DeleteThenRecreateReplaysActualFile)
{
    inverted_index::DeferredPathQueue queue;
    const fs::path path = fs::temp_directory_path() / "deferred_recreated.txt";
    std::error_code error;
    fs::remove(path, error);
    queue.defer(path.wstring());
    { std::ofstream file(path); file << "new content"; }

    bool replayedAsUpdate = false;
    queue.drainOnce([&](const std::wstring& item) {
        replayedAsUpdate = fs::is_regular_file(item);
    });
    EXPECT_TRUE(replayedAsUpdate);
    fs::remove(path, error);
}

TEST(DeferredPathQueue, BusyReplayDefersOnceWithoutLooping)
{
    inverted_index::DeferredPathQueue queue;
    queue.defer(L"busy.txt");
    std::size_t calls = 0;
    queue.drainOnce([&](const std::wstring& path) {
        ++calls;
        queue.defer(path);
    });
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(queue.size(), 1u);
}

} // namespace
