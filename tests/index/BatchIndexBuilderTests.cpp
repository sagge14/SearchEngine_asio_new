#include <gtest/gtest.h>

#include "Index/Batch/BatchIndexBuilder.h"
#include "Index/Batch/BoundedByteQueue.h"
#include "Index/Batch/FullIndexStrategy.h"
#include "MyUtils/OEMCase.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;

class TemporaryCorpus final {
public:
    TemporaryCorpus()
    {
        static std::atomic<unsigned long long> sequence{0};
        path_ = fs::temp_directory_path() /
            ("searchengine_batch_index_test_" +
             std::to_string(sequence.fetch_add(1)));
        fs::create_directories(path_);
    }

    ~TemporaryCorpus()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    fs::path write(const std::string& name, const std::string& content) const
    {
        const fs::path path = path_ / name;
        std::ofstream stream(path, std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        return path;
    }

private:
    fs::path path_;
};

#ifdef _WIN32
class ExclusiveFileLock final {
public:
    explicit ExclusiveFileLock(const fs::path& path)
    {
        error_ = _wsopen_s(
            &descriptor_,
            path.c_str(),
            _O_RDONLY | _O_BINARY,
            _SH_DENYRW,
            _S_IREAD);
    }

    ~ExclusiveFileLock()
    {
        if (descriptor_ >= 0)
            _close(descriptor_);
    }

    [[nodiscard]] bool acquired() const noexcept
    {
        return error_ == 0 && descriptor_ >= 0;
    }

private:
    int descriptor_{-1};
    errno_t error_{};
};
#endif

void initializeAsciiCaseTable()
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
}

void expectSamePostingList(const PostingList& left, const PostingList& right)
{
    ASSERT_EQ(left.size(), right.size());
    auto leftIt = left.begin();
    auto rightIt = right.begin();
    for (; leftIt != left.end(); ++leftIt, ++rightIt) {
        EXPECT_EQ(leftIt->fileId, rightIt->fileId);
        EXPECT_EQ(leftIt->cnt, rightIt->cnt);
    }
}

using LogicalIndex =
    std::map<std::string, std::map<uint32_t, uint16_t>>;

LogicalIndex logicalIndex(
    const inverted_index::batch::BatchIndexSnapshot& snapshot)
{
    LogicalIndex result;
    for (std::size_t wordId = 0; wordId < snapshot.idToWord.size(); ++wordId) {
        auto& documents = result[snapshot.idToWord[wordId]];
        for (const Posting& posting : snapshot.postings[wordId])
            documents.emplace(posting.fileId, posting.cnt);
    }
    return result;
}

std::size_t postingRows(
    const inverted_index::batch::BatchIndexSnapshot& snapshot)
{
    std::size_t result = 0;
    for (const PostingList& postings : snapshot.postings)
        result += postings.size();
    return result;
}

} // namespace

TEST(FullIndexStrategy, ParsesOnlySupportedStableNames)
{
    EXPECT_EQ(
        inverted_index::parseFullIndexStrategy("legacy"),
        inverted_index::FullIndexStrategy::Legacy);
    EXPECT_EQ(
        inverted_index::parseFullIndexStrategy("batch"),
        inverted_index::FullIndexStrategy::Batch);
    EXPECT_FALSE(inverted_index::parseFullIndexStrategy("Batch"));
    EXPECT_FALSE(inverted_index::parseFullIndexStrategy(""));
}

TEST(BatchIndexBuilder, ProducesSameLogicalIndexWithOneOrManyWorkers)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    const std::vector<std::wstring> paths{
        corpus.write("one.txt", "alpha beta alpha").wstring(),
        corpus.write("two.txt", "beta gamma").wstring(),
        corpus.write("empty.txt", "").wstring(),
        corpus.write("three.txt", "gamma alpha delta").wstring()};

    const inverted_index::batch::BatchIndexSnapshot single =
        inverted_index::batch::BatchIndexBuilder({1, 1, 8}).build(paths);
    const inverted_index::batch::BatchIndexSnapshot parallel =
        inverted_index::batch::BatchIndexBuilder({2, 4, 8}).build(paths);

    ASSERT_EQ(single.fileErrors.size(), 0u);
    ASSERT_EQ(parallel.fileErrors.size(), 0u);
    EXPECT_EQ(single.indexedFiles, paths.size());
    EXPECT_EQ(parallel.indexedFiles, paths.size());
    EXPECT_EQ(single.idToWord, parallel.idToWord);
    EXPECT_EQ(single.wordToId, parallel.wordToId);
    ASSERT_EQ(single.postings.size(), parallel.postings.size());
    for (std::size_t index = 0; index < single.postings.size(); ++index)
        expectSamePostingList(single.postings[index], parallel.postings[index]);

    EXPECT_EQ(single.idToWord.size(), 4u);
    EXPECT_EQ(single.idToWord[0], "ALPHA");
    EXPECT_EQ(single.idToWord[1], "BETA");
    EXPECT_EQ(single.idToWord[2], "DELTA");
    EXPECT_EQ(single.idToWord[3], "GAMMA");

    const uint32_t alphaId = single.wordToId.at("ALPHA");
    ASSERT_EQ(single.postings[alphaId].size(), 2u);
    const uint16_t* firstCount = single.postings[alphaId].find(0);
    const uint16_t* thirdCount = single.postings[alphaId].find(3);
    ASSERT_NE(firstCount, nullptr);
    ASSERT_NE(thirdCount, nullptr);
    EXPECT_EQ(*firstCount, 2u);
    EXPECT_EQ(*thirdCount, 1u);
}

TEST(BatchIndexBuilder, KeepsFrequentWordPostingsSortedAndWordRefsUnique)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    std::vector<std::wstring> paths;
    for (std::size_t index = 0; index < 12; ++index) {
        paths.push_back(
            corpus.write(
                "common_" + std::to_string(index) + ".txt",
                "common term" + std::to_string(index))
                .wstring());
    }

    const auto snapshot =
        inverted_index::batch::BatchIndexBuilder({2, 4, 64u * 1024u})
            .build(paths);

    ASSERT_TRUE(snapshot.fileErrors.empty());
    const uint32_t commonId = snapshot.wordToId.at("COMMON");
    const PostingList& postings = snapshot.postings[commonId];
    ASSERT_EQ(postings.size(), paths.size());

    uint32_t expectedFileId = 0;
    for (const Posting& posting : postings) {
        EXPECT_EQ(posting.fileId, expectedFileId++);
        EXPECT_EQ(posting.cnt, 1u);
    }

    for (uint32_t fileId = 0; fileId < paths.size(); ++fileId) {
        const auto& refs = snapshot.wordRefs.at(fileId);
        EXPECT_EQ(std::count(refs.begin(), refs.end(), commonId), 1);
    }
}

TEST(BatchIndexBuilder, MatchesReferenceTokensAndIndexMetrics)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;

    // CP866 bytes for uppercase "PRIVET". Uppercase bytes make the test
    // independent of an external OEM866.INI while still exercising the
    // tokenizer's 8-bit Cyrillic word range.
    const std::string oem866Word{"\x8f\x90\x88\x82\x85\x92", 6};
    std::string acrossBoundary(64u * 1024u - 3u, ' ');
    acrossBoundary += "boundary word ";

    const std::vector<std::wstring> paths{
        corpus.write("ascii.txt", "alpha, beta! alpha? 123.\n").wstring(),
        corpus.write("oem866.txt", oem866Word + " " + oem866Word + " ").wstring(),
        corpus.write("boundary.txt", acrossBoundary).wstring(),
        corpus.write("empty.txt", "").wstring()};

    const auto snapshot =
        inverted_index::batch::BatchIndexBuilder({2, 3, 16u * 1024u})
            .build(paths);

    ASSERT_TRUE(snapshot.fileErrors.empty());
    EXPECT_EQ(snapshot.documents.size(), paths.size());
    EXPECT_EQ(snapshot.indexedFiles, paths.size());
    EXPECT_EQ(snapshot.idToWord.size(), 6u);
    EXPECT_EQ(postingRows(snapshot), 6u);

    const LogicalIndex expected{
        {"123", {{0u, 1u}}},
        {"ALPHA", {{0u, 2u}}},
        {"BETA", {{0u, 1u}}},
        {"BOUNDARY", {{2u, 1u}}},
        {"WORD", {{2u, 1u}}},
        {oem866Word, {{1u, 2u}}}};
    EXPECT_EQ(logicalIndex(snapshot), expected);
}

TEST(BatchIndexBuilder, KeepsCurrentExact64KiBTokenizerBoundaryBehavior)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    std::string content(64u * 1024u, ' ');
    content.replace(content.size() - 4, 4, "tail");
    const std::vector<std::wstring> paths{
        corpus.write("boundary.txt", content).wstring()};

    const inverted_index::batch::BatchIndexSnapshot snapshot =
        inverted_index::batch::BatchIndexBuilder({1, 2, 64u * 1024u})
            .build(paths);

    // The legacy reader does not flush carry after a final full 64 KiB read.
    // The batch implementation intentionally preserves this until the shared
    // tokenizer contract is changed for both strategies in a separate patch.
    EXPECT_TRUE(snapshot.idToWord.empty());
    EXPECT_EQ(snapshot.indexedFiles, 1u);
}

TEST(BatchIndexBuilder, StreamsFilesLargerThanTheConfiguredQueue)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    std::string content(512u * 1024u, ' ');
    content.replace(0, 5, "alpha");
    content.replace(content.size() - 6, 5, "omega");
    const std::vector<std::wstring> paths{
        corpus.write("large.txt", content).wstring()};

    const inverted_index::batch::BatchIndexSnapshot snapshot =
        inverted_index::batch::BatchIndexBuilder({1, 2, 32u * 1024u})
            .build(paths);

    ASSERT_TRUE(snapshot.fileErrors.empty());
    EXPECT_EQ(snapshot.bytesRead, content.size());
    EXPECT_EQ(snapshot.indexedFiles, 1u);
    EXPECT_EQ(snapshot.idToWord.size(), 2u);
    EXPECT_TRUE(snapshot.wordToId.contains("ALPHA"));
    EXPECT_TRUE(snapshot.wordToId.contains("OMEGA"));
}

TEST(BatchIndexBuilder, SaturatesAStoredTermFrequency)
{
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    std::string content;
    content.reserve(70'000u * 2u);
    for (std::size_t index = 0; index < 70'000u; ++index)
        content += "a ";
    const std::vector<std::wstring> paths{
        corpus.write("frequent.txt", content).wstring()};

    const inverted_index::batch::BatchIndexSnapshot snapshot =
        inverted_index::batch::BatchIndexBuilder({1, 1, 64u * 1024u})
            .build(paths);

    ASSERT_EQ(snapshot.idToWord.size(), 1u);
    const uint16_t* count = snapshot.postings.front().find(0);
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(*count, std::numeric_limits<uint16_t>::max());
}

TEST(BatchIndexBuilder, FileErrorOwnsTheOriginalPathAndMessage)
{
#ifdef _WIN32
    initializeAsciiCaseTable();
    TemporaryCorpus corpus;
    const fs::path path = corpus.write("locked.txt", "alpha");
    const ExclusiveFileLock lock(path);
    ASSERT_TRUE(lock.acquired());

    const auto snapshot =
        inverted_index::batch::BatchIndexBuilder({1, 1, 64u * 1024u})
            .build({path.wstring()});

    ASSERT_EQ(snapshot.fileErrors.size(), 1u);
    EXPECT_EQ(snapshot.fileErrors.front().fileId, 0u);
    EXPECT_EQ(snapshot.fileErrors.front().path, path.wstring());
    EXPECT_EQ(snapshot.fileErrors.front().message, "file not found");
    EXPECT_EQ(snapshot.indexedFiles, 0u);
#else
    GTEST_SKIP() << "The deterministic sharing-violation check is Windows-only";
#endif
}

TEST(DocPaths, KeepsPathPointersValidAfterCopyMoveAndRebuildFromRows)
{
    TemporaryCorpus corpus;
    const fs::path first = corpus.write("one.txt", "one");
    const fs::path second = corpus.write("two.txt", "two");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};

    DocPaths original;
    const UpdatePack update = original.getUpdate(paths);
    ASSERT_EQ(update.added, (std::vector<uint32_t>{0u, 1u}));
    original.markRemoved(1u);

    const auto expectState = [&](const DocPaths& documents) {
        ASSERT_EQ(documents.size(), 2u);
        EXPECT_EQ(documents.pathById(0u), first.wstring());
        EXPECT_EQ(documents.pathById(1u), second.wstring());
        EXPECT_FALSE(documents.isDeleted(0u));
        EXPECT_TRUE(documents.isDeleted(1u));
    };

    DocPaths copyConstructed(original);
    expectState(copyConstructed);

    DocPaths copyAssigned;
    copyAssigned = original;
    expectState(copyAssigned);

    DocPaths moveConstructed(std::move(copyConstructed));
    expectState(moveConstructed);

    DocPaths moveAssigned;
    moveAssigned = std::move(copyAssigned);
    expectState(moveAssigned);

    std::vector<DocPaths::RawRow> rows = moveAssigned.exportRows();
    DocPaths rebuilt;
    rebuilt.rebuildFromRows(std::move(rows));
    expectState(rebuilt);

    std::size_t visited = 0;
    rebuilt.forEachRow(
        [&](uint32_t id,
            const std::wstring& path,
            int64_t,
            uint64_t,
            bool) {
            ASSERT_LT(id, 2u);
            EXPECT_EQ(&path, &rebuilt.pathById(id));
            ++visited;
        });
    EXPECT_EQ(visited, 2u);
}

TEST(BoundedByteQueue, RejectsAnItemLargerThanItsCapacity)
{
    inverted_index::batch::BoundedByteQueue<std::string> queue(4);
    EXPECT_THROW(queue.push("oversized", 5), std::length_error);
}
