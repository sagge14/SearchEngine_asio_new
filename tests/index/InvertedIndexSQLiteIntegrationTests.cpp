#include <gtest/gtest.h>

#include "Index/InvertedIndex.h"
#include "Index/SQLiteIndexSerializer.h"
#include "MyUtils/OEMCase.h"
#include "SQLite/sqlite3.h"

#include <boost/asio/executor_work_guard.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace inverted_index {

struct InvertedIndexTestAccess {
    static std::pair<size_t, size_t> representationSlots(
        InvertedIndex& index)
    {
        std::lock_guard<std::mutex> lock(index.mapMutex);
        return {
            index.dictionary.size(),
            index.dictionaryChunks.size() * InvertedIndex::CHUNK_SIZE};
    }

    static void compactAndWait(InvertedIndex& index, double thresholdPercent)
    {
        index.compact(thresholdPercent);
        index.waitForIdle();
    }

    static void writeConflictingCatalogRow(InvertedIndex& index)
    {
        index.ensureSerializer();
        const std::wstring existingPath = index.filePathById(0u);
        index.serializer_->writeFile(
            999u, existingPath, 0, 0, {}, {}, true);
        index.serializer_->flushPending();
    }
};

} // namespace inverted_index

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

    fs::path write(const fs::path& relativePath, const std::string& content) const
    {
        const fs::path path = root_ / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("cannot create integration test file");
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream)
            throw std::runtime_error("cannot write integration test file");
        return path;
    }

    [[nodiscard]] fs::path database(
        const std::string& name = "index.sqlite") const
    {
        return root_ / name;
    }

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

inverted_index::IndexStorageConfig sqliteBatchConfig(
    const fs::path& path,
    inverted_index::DocumentCatalogStorage catalogStorage =
        inverted_index::DocumentCatalogStorage::Memory,
    inverted_index::FullIndexStrategy strategy =
        inverted_index::FullIndexStrategy::Batch)
{
    inverted_index::IndexStorageConfig config;
    config.kind = inverted_index::IndexSerializationKind::SQLite;
    config.path = path.string();
    config.sqliteMirrorFlushIntervalSec = 0.01;
    config.sqliteMirrorMaxPendingOps = 1;
    config.fullIndexStrategy = strategy;
    config.documentCatalogStorage = catalogStorage;
    config.batchReaderThreads = 1;
    config.batchIndexerThreads = 2;
    config.batchQueueMemoryBytes = 64u * 1024u;
    return config;
}

std::int64_t scalarInt64(const fs::path& database, const char* sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(
            database.string().c_str(),
            &db,
            SQLITE_OPEN_READONLY,
            nullptr) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        throw std::runtime_error("cannot open integration SQLite database");
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("cannot prepare integration SQLite query");
    }
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        throw std::runtime_error("integration SQLite query returned no row");
    }
    const std::int64_t value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

void executeSql(const fs::path& database, const char* sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(
            database.string().c_str(),
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        throw std::runtime_error("cannot open integration SQLite database");
    }
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : sqlite3_errmsg(db);
    if (error)
        sqlite3_free(error);
    sqlite3_close(db);
    if (rc != SQLITE_OK)
        throw std::runtime_error("integration SQLite exec failed: " + message);
}

void expectPosting(
    inverted_index::InvertedIndex& index,
    const std::string& word,
    std::initializer_list<std::pair<uint32_t, uint32_t>> expected)
{
    const PostingList postings = index.getWordCount(word);
    ASSERT_EQ(postings.size(), expected.size()) << word;
    for (const auto& [fileId, count] : expected) {
        const uint16_t* actual = postings.find(fileId);
        ASSERT_NE(actual, nullptr) << word << " file=" << fileId;
        EXPECT_EQ(static_cast<uint32_t>(*actual), count)
            << word << " file=" << fileId;
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

struct LogicalIndexSnapshot {
    PostingList common;
    PostingList rare;
    std::vector<std::optional<inverted_index::DocumentRecord>> documents;
};

LogicalIndexSnapshot captureLogicalIndex(inverted_index::InvertedIndex& index)
{
    return {
        index.getWordCount("COMMON"),
        index.getWordCount("RARE"),
        index.documentsByIds({0u, 1u})};
}

void expectSameLogicalIndex(
    const LogicalIndexSnapshot& expected,
    const LogicalIndexSnapshot& actual)
{
    const auto expectSamePostingList = [](const PostingList& left,
                                          const PostingList& right) {
        ASSERT_EQ(left.size(), right.size());
        auto leftIt = left.begin();
        auto rightIt = right.begin();
        for (; leftIt != left.end(); ++leftIt, ++rightIt) {
            EXPECT_EQ(leftIt->fileId, rightIt->fileId);
            EXPECT_EQ(leftIt->cnt, rightIt->cnt);
        }
    };
    expectSamePostingList(expected.common, actual.common);
    expectSamePostingList(expected.rare, actual.rare);
    ASSERT_EQ(actual.documents.size(), expected.documents.size());
    for (std::size_t index = 0; index < expected.documents.size(); ++index) {
        ASSERT_EQ(
            actual.documents[index].has_value(),
            expected.documents[index].has_value());
        if (!expected.documents[index])
            continue;
        EXPECT_EQ(actual.documents[index]->id, expected.documents[index]->id);
        EXPECT_EQ(actual.documents[index]->path, expected.documents[index]->path);
        EXPECT_EQ(actual.documents[index]->deleted, expected.documents[index]->deleted);
    }
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
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
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
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
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
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
    }
}

TEST(InvertedIndexSQLiteIntegration, SwitchesCatalogBackendsWithoutChangingIds)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "alpha beta alpha");
    const fs::path second = temporary.write("two.txt", "beta gamma");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};

    const auto memoryConfig = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::Memory);
    const auto sqliteConfig = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::SQLite);

    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, memoryConfig);
        index.updateDocumentBase(paths).get();
        index.waitForIdle();
        EXPECT_TRUE(index.documentPathsLoadedInMemory());
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, sqliteConfig);
        expectInitialIndex(index);
        EXPECT_FALSE(index.documentPathsLoadedInMemory());
        EXPECT_EQ(index.documentCatalogCacheCapacity(), 0u);
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, memoryConfig);
        expectInitialIndex(index);
        EXPECT_TRUE(index.documentPathsLoadedInMemory());
        EXPECT_EQ(index.filePathById(0u), first.wstring());
        EXPECT_EQ(index.filePathById(1u), second.wstring());
    }
}

TEST(InvertedIndexSQLiteIntegration, BatchSaveReloadMatchesInBothCatalogModes)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "common common rare");
    const fs::path second = temporary.write("two.txt", "common");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};

    const auto memoryConfig = sqliteBatchConfig(
        temporary.database("memory.sqlite"),
        inverted_index::DocumentCatalogStorage::Memory);
    const auto sqliteConfig = sqliteBatchConfig(
        temporary.database("sqlite.sqlite"),
        inverted_index::DocumentCatalogStorage::SQLite);

    LogicalIndexSnapshot memoryBuilt;
    LogicalIndexSnapshot sqliteBuilt;
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, memoryConfig);
        index.updateDocumentBase(paths).get();
        index.waitForIdle();
        memoryBuilt = captureLogicalIndex(index);
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, sqliteConfig);
        index.updateDocumentBase(paths).get();
        index.waitForIdle();
        sqliteBuilt = captureLogicalIndex(index);
        EXPECT_FALSE(index.documentPathsLoadedInMemory());
    }
    expectSameLogicalIndex(memoryBuilt, sqliteBuilt);

    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, memoryConfig);
        expectSameLogicalIndex(memoryBuilt, captureLogicalIndex(index));
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, sqliteConfig);
        expectSameLogicalIndex(sqliteBuilt, captureLogicalIndex(index));
        EXPECT_FALSE(index.documentPathsLoadedInMemory());
    }
}

TEST(InvertedIndexSQLiteIntegration, SQLiteCatalogSupportsLegacyPointLifecycle)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "alpha beta alpha");
    const fs::path second = temporary.write("two.txt", "beta gamma");
    const std::vector<std::wstring> paths{first.wstring()};
    const auto config = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::SQLite,
        inverted_index::FullIndexStrategy::Legacy);

    AsyncIndexRuntime runtime;
    inverted_index::InvertedIndex index(
        runtime.cpu_, runtime.io_, 2, 10, config);
    index.updateDocumentBase(paths).get();
    index.waitForIdle();
    ASSERT_FALSE(index.documentPathsLoadedInMemory());
    expectPosting(index, "ALPHA", {{0u, 2u}});

    ASSERT_TRUE(index.enqueueFileUpdate(second.wstring()));
    index.waitForIdle();
    EXPECT_EQ(index.filePathById(1u), second.wstring());
    expectPosting(index, "BETA", {{0u, 1u}, {1u, 1u}});

    temporary.write("two.txt", "gamma gamma pointupdate");
    ASSERT_TRUE(index.enqueueFileUpdate(second.wstring()));
    index.waitForIdle();
    EXPECT_EQ(index.filePathById(1u), second.wstring());
    expectPosting(index, "GAMMA", {{1u, 2u}});
    expectPosting(index, "POINTUPDATE", {{1u, 1u}});
    EXPECT_EQ(index.getWordCount("BETA").size(), 1u);

    ASSERT_TRUE(index.enqueueFileDeletion(first.wstring()));
    index.waitForIdle();
    EXPECT_TRUE(index.isFileDeleted(0u));
    EXPECT_EQ(index.filePathById(0u), first.wstring());
    expectPosting(index, "ALPHA", {{0u, 2u}});

    temporary.write("one.txt", "delta delta epsilon reactivated");
    ASSERT_TRUE(index.enqueueFileUpdate(first.wstring()));
    index.waitForIdle();
    EXPECT_FALSE(index.isFileDeleted(0u));
    EXPECT_EQ(index.filePathById(0u), first.wstring());
    EXPECT_TRUE(index.getWordCount("ALPHA").empty());
    expectPosting(index, "DELTA", {{0u, 2u}});
    expectPosting(index, "REACTIVATED", {{0u, 1u}});
}

TEST(InvertedIndexSQLiteIntegration, PreservesUnicodeAndLongPathsAcrossReload)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path relative =
        fs::path(L"unicode_каталог") /
        (std::wstring(120, L'д') + L"_файл.txt");
    const fs::path document = temporary.write(relative, "unicodepath");
    ASSERT_GT(document.wstring().size(), 170u);

    const auto sqliteConfig = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::SQLite);
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, sqliteConfig);
        index.updateDocumentBase({document.wstring()}).get();
        index.waitForIdle();
        EXPECT_EQ(index.filePathById(0u), document.wstring());
        expectPosting(index, "UNICODEPATH", {{0u, 1u}});
    }
    {
        AsyncIndexRuntime runtime;
        inverted_index::InvertedIndex index(
            runtime.cpu_, runtime.io_, 2, 10, sqliteConfig);
        EXPECT_EQ(index.filePathById(0u), document.wstring());
        expectPosting(index, "UNICODEPATH", {{0u, 1u}});
        EXPECT_FALSE(index.documentPathsLoadedInMemory());
    }
}

TEST(InvertedIndexSQLiteIntegration, RejectsDuplicatePathsAndOrphanPostings)
{
    TemporaryIndex temporary;
    const fs::path duplicateDatabase = temporary.database("duplicate.sqlite");
    executeSql(
        duplicateDatabase,
        "CREATE TABLE docs(doc_id INTEGER PRIMARY KEY,path TEXT NOT NULL,"
        "mtime_ticks INTEGER NOT NULL,size_int64 INTEGER NOT NULL,"
        "deleted INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO docs VALUES(0,'same',0,0,0);"
        "INSERT INTO docs VALUES(1,'same',0,0,0);");

    AsyncIndexRuntime duplicateRuntime;
    EXPECT_THROW(
        inverted_index::InvertedIndex(
            duplicateRuntime.cpu_,
            duplicateRuntime.io_,
            2,
            10,
            sqliteBatchConfig(
                duplicateDatabase,
                inverted_index::DocumentCatalogStorage::SQLite)),
        std::exception);

    const fs::path orphanDatabase = temporary.database("orphan.sqlite");
    executeSql(
        orphanDatabase,
        "CREATE TABLE docs(doc_id INTEGER PRIMARY KEY,path TEXT NOT NULL,"
        "mtime_ticks INTEGER NOT NULL,size_int64 INTEGER NOT NULL,"
        "deleted INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE postings(word_id INTEGER NOT NULL,doc_id INTEGER NOT NULL,"
        "cnt INTEGER NOT NULL,PRIMARY KEY(word_id,doc_id)) WITHOUT ROWID;"
        "INSERT INTO postings VALUES(0,77,1);");

    AsyncIndexRuntime orphanRuntime;
    EXPECT_THROW(
        inverted_index::InvertedIndex(
            orphanRuntime.cpu_,
            orphanRuntime.io_,
            2,
            10,
            sqliteBatchConfig(
                orphanDatabase,
                inverted_index::DocumentCatalogStorage::Memory)),
        std::exception);
}

TEST(InvertedIndexSQLiteIntegration, BatchFetchSplitsLargeTopNAndKeepsOrder)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    std::vector<std::wstring> paths;
    constexpr std::size_t documentCount = 1005;
    paths.reserve(documentCount);
    for (std::size_t index = 0; index < documentCount; ++index) {
        paths.push_back(
            temporary.write(
                "doc_" + std::to_string(index) + ".txt", "common")
                .wstring());
    }
    const auto config = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::SQLite);

    AsyncIndexRuntime runtime;
    inverted_index::InvertedIndex index(
        runtime.cpu_, runtime.io_, 2, 10, config);
    index.updateDocumentBase(paths).get();
    index.waitForIdle();

    std::vector<uint32_t> ids;
    ids.reserve(documentCount);
    for (std::size_t index = 0; index < documentCount; ++index)
        ids.push_back(static_cast<uint32_t>(documentCount - index - 1));
    const auto documents = index.documentsByIds(ids);
    ASSERT_EQ(documents.size(), ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        ASSERT_TRUE(documents[index].has_value());
        EXPECT_EQ(documents[index]->id, ids[index]);
        EXPECT_EQ(documents[index]->path, paths[ids[index]]);
    }
}

TEST(InvertedIndexSQLiteIntegration, WriterFailurePropagatesWithoutPartialDoc)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "alpha beta alpha");
    const fs::path second = temporary.write("two.txt", "beta gamma");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};
    const auto config = sqliteBatchConfig(
        temporary.database(),
        inverted_index::DocumentCatalogStorage::SQLite);

    AsyncIndexRuntime runtime;
    inverted_index::InvertedIndex index(
        runtime.cpu_, runtime.io_, 2, 10, config);
    index.updateDocumentBase(paths).get();
    index.waitForIdle();

    EXPECT_THROW(
        inverted_index::InvertedIndexTestAccess::writeConflictingCatalogRow(
            index),
        std::exception);
    EXPECT_EQ(
        scalarInt64(
            temporary.database(),
            "SELECT COUNT(*) FROM docs WHERE doc_id=999;"),
        0);
    EXPECT_EQ(
        scalarInt64(
            temporary.database(),
            "SELECT COUNT(*) FROM postings WHERE doc_id=999;"),
        0);
}

TEST(InvertedIndexSQLiteIntegration, CoalescedWriteThenDeleteKeepsEternalTrace)
{
    TemporaryIndex temporary;
    inverted_index::LiveMirrorConfig config;
    config.flushIntervalSec = 60.0;
    config.maxPendingOps = 1000;
    config.documentCatalogStorage =
        inverted_index::DocumentCatalogStorage::SQLite;

    inverted_index::SQLiteIndexSerializer serializer(
        temporary.database().string(), config);
    serializer.openLive();
    serializer.writeFile(
        42u, L"C:\\catalog\\eternal.txt", 123, 456, {}, {}, false);
    serializer.markFileDeleted(42u);
    serializer.flushPending();

    EXPECT_EQ(
        scalarInt64(
            temporary.database(),
            "SELECT COUNT(*) FROM docs WHERE doc_id=42;"),
        1);
    EXPECT_EQ(
        scalarInt64(
            temporary.database(),
            "SELECT deleted FROM docs WHERE doc_id=42;"),
        1);
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

TEST(InvertedIndexCompaction, NoOpLegacyUpdateKeepsOnePostingRepresentation)
{
    OEMCase::init("__searchengine_test_missing_oem866_table__.ini");
    TemporaryIndex temporary;
    const fs::path first = temporary.write("one.txt", "alpha beta alpha");
    const fs::path second = temporary.write("two.txt", "beta gamma");
    const std::vector<std::wstring> paths{first.wstring(), second.wstring()};

    AsyncIndexRuntime runtime;
    inverted_index::InvertedIndex index(
        runtime.cpu_, runtime.io_, 2, 10, sqliteBatchConfig(temporary.database()));

    index.updateDocumentBase(paths).get();
    index.waitForIdle();
    const auto [batchFlatSlots, batchChunkSlots] =
        inverted_index::InvertedIndexTestAccess::representationSlots(index);
    ASSERT_GT(batchFlatSlots, 0u);
    ASSERT_EQ(batchChunkSlots, 0u);

    index.updateDocumentBase(paths).get();
    index.waitForIdle();
    inverted_index::InvertedIndexTestAccess::compactAndWait(index, 5.0);

    const auto [legacyFlatSlots, legacyChunkSlots] =
        inverted_index::InvertedIndexTestAccess::representationSlots(index);
    EXPECT_EQ(legacyFlatSlots, 0u);
    EXPECT_GT(legacyChunkSlots, 0u);
    expectInitialIndex(index);
}

} // namespace
