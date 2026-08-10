#include <gtest/gtest.h>

#include "Index/InvertedIndex.h"
#include "MyUtils/OEMCase.h"

#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TemporaryIndex final {
public:
    TemporaryIndex()
    {
        static std::atomic<unsigned long long> sequence{0};
        root_ = fs::temp_directory_path() /
            ("searchengine_index_integration_" +
             std::to_string(sequence.fetch_add(1)));
        fs::create_directories(root_);
    }

    ~TemporaryIndex()
    {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    fs::path write(const std::string& name, const std::string& content) const
    {
        const fs::path path = root_ / name;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        return path;
    }

    [[nodiscard]] fs::path database() const { return root_ / "index.sqlite"; }

private:
    fs::path root_;
};

class AsyncIndexRuntime final {
public:
    AsyncIndexRuntime()
        : work_(boost::asio::make_work_guard(io_))
        , ioThread_([this] { io_.run(); })
    {
    }

    ~AsyncIndexRuntime()
    {
        work_.reset();
        io_.stop();
        if (ioThread_.joinable())
            ioThread_.join();
        cpu_.join();
    }

    boost::asio::thread_pool cpu_{4};
    boost::asio::io_context io_;

private:
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_;
    std::thread ioThread_;
};

inverted_index::IndexStorageConfig sqliteBatchConfig(const fs::path& path)
{
    inverted_index::IndexStorageConfig config;
    config.kind = inverted_index::IndexSerializationKind::SQLite;
    config.path = path.string();
    config.sqliteMirrorFlushIntervalSec = 0.01;
    config.sqliteMirrorMaxPendingOps = 1;
    config.fullIndexStrategy = inverted_index::FullIndexStrategy::Batch;
    config.batchReaderThreads = 1;
    config.batchIndexerThreads = 2;
    config.batchQueueMemoryBytes = 64u * 1024u;
    return config;
}

void expectPosting(
    inverted_index::InvertedIndex& index,
    const std::string& word,
    std::initializer_list<std::pair<uint32_t, uint16_t>> expected)
{
    const PostingList postings = index.getWordCount(word);
    ASSERT_EQ(postings.size(), expected.size()) << word;
    for (const auto& [fileId, count] : expected) {
        const uint16_t* actual = postings.find(fileId);
        ASSERT_NE(actual, nullptr) << word << " file=" << fileId;
        EXPECT_EQ(*actual, count) << word << " file=" << fileId;
    }
}

void expectInitialIndex(inverted_index::InvertedIndex& index)
{
    const auto stats = index.getStats();
    EXPECT_EQ(stats.totalFiles, 2u);
    EXPECT_EQ(stats.uniqueWords, 3u);
    expectPosting(index, "ALPHA", {{0u, 2u}});
    expectPosting(index, "BETA", {{0u, 1u}, {1u, 1u}});
    expectPosting(index, "GAMMA", {{1u, 1u}});
}

TEST(InvertedIndexSQLiteIntegration, BatchReloadLegacyUpdateAndReload)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "alpha beta alpha");
    const fs::path second = temporary.write("two.txt", "beta gamma");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};
    const auto config = sqliteBatchConfig(temporary.database());

    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(runtime.cpu_, runtime.io_, 2, 10, config);
        index.updateDocumentBase(paths).get();
        index.waitForIdle();
        expectInitialIndex(index);
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(runtime.cpu_, runtime.io_, 2, 10, config);
        expectInitialIndex(index);

        temporary.write("one.txt", "delta delta epsilon changed");
        index.updateDocumentBase(paths).get();
        index.waitForIdle();

        const auto stats = index.getStats();
        EXPECT_EQ(stats.totalFiles, 2u);
        EXPECT_TRUE(index.getWordCount("ALPHA").empty());
        expectPosting(index, "BETA", {{1u, 1u}});
        expectPosting(index, "GAMMA", {{1u, 1u}});
        expectPosting(index, "DELTA", {{0u, 2u}});
        expectPosting(index, "EPSILON", {{0u, 1u}});
        expectPosting(index, "CHANGED", {{0u, 1u}});
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(runtime.cpu_, runtime.io_, 2, 10, config);
        EXPECT_TRUE(index.getWordCount("ALPHA").empty());
        expectPosting(index, "BETA", {{1u, 1u}});
        expectPosting(index, "GAMMA", {{1u, 1u}});
        expectPosting(index, "DELTA", {{0u, 2u}});
        expectPosting(index, "EPSILON", {{0u, 1u}});
        expectPosting(index, "CHANGED", {{0u, 1u}});
    }
}

TEST(InvertedIndexCoordination, AcceptedPointUpdateForcesFallbackAfterDrain)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path point = temporary.write("point.txt", "pointonly");
    const fs::path corpus = temporary.write("corpus.txt", "corpusonly");
    const auto config = sqliteBatchConfig(temporary.database());

    AsyncIndexRuntime runtime;
    inverted_index::InvertedIndex index(runtime.cpu_, runtime.io_, 2, 10, config);
    ASSERT_TRUE(index.enqueueFileUpdate(point.wstring()));

    index.updateDocumentBase({corpus.wstring()}).get();
    index.waitForIdle();

    expectPosting(index, "POINTONLY", {{0u, 1u}});
    expectPosting(index, "CORPUSONLY", {{1u, 1u}});
    EXPECT_TRUE(index.isFileDeleted(0u));
}

} // namespace
