#pragma once

#include "IIndexSerializer.h"
#include "DocumentCatalogStorage.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace inverted_index {

struct LiveMirrorConfig {
    double flushIntervalSec = 2.0;
    int maxPendingOps = 500;
    int loadThreads = 4;
    bool precountPostings = false;
    DocumentCatalogStorage documentCatalogStorage =
        DocumentCatalogStorage::Memory;
};

struct LiveMirrorOp {
    enum class Kind { Write, MarkDeleted };
    Kind kind = Kind::Write;
    uint32_t fileId = 0;
    std::wstring path;
    int64_t mtimeTicks = 0;
    uint64_t size = 0;
    std::vector<std::pair<uint32_t, uint16_t>> widCounts;
    std::vector<std::pair<uint32_t, std::string>> newWords;
    bool markDeletedAfterWrite = false;
};

class SQLiteIndexSerializer final : public IIndexSerializer {
public:
    explicit SQLiteIndexSerializer(std::string path,
                                   LiveMirrorConfig config = {});
    ~SQLiteIndexSerializer() override;

    [[nodiscard]] std::string kind() const override;
    [[nodiscard]] bool exists() const override;

    void save(const InvertedIndex& idx) override;
    void load(InvertedIndex& idx) override;

    [[nodiscard]] bool supportsLiveUpdates() const override { return true; }
    void openLive() override;
    void writeFile(uint32_t fileId,
                   const std::wstring& path,
                   int64_t mtimeTicks,
                   uint64_t size,
                   const std::vector<std::pair<uint32_t, uint16_t>>& widCounts,
                   const std::vector<std::pair<uint32_t, std::string>>& newWords,
                   bool wasUpdate) override;
    void markFileDeleted(uint32_t fileId) override;
    void flushPending() override;
    void checkpoint() override;

private:
    std::string path_;
    LiveMirrorConfig config_;

    sqlite3* db_{nullptr};

    void open();
    void close() noexcept;

    void exec(sqlite3* db, const char* sql);
    void initSchema(sqlite3* db);
    void migratePostingsToWithoutRowid(sqlite3* db);
    [[nodiscard]] bool tableHasColumn(sqlite3* db,
                                      const char* table,
                                      const char* column);
    [[nodiscard]] std::string tableSql(sqlite3* db, const char* table);

    sqlite3_stmt* prepare(sqlite3* db, const char* sql);
    void finalize(sqlite3_stmt* st) noexcept;

    void beginImmediate(sqlite3* db);
    void commit(sqlite3* db);
    void rollback(sqlite3* db) noexcept;

    /* --- live writer thread --------------------------------------------- */
    void startWriter();
    void stopWriter();
    void writerLoop();
    void openLiveDb();
    void closeLiveDb() noexcept;
    void prepareLiveStatements();
    void finalizeLiveStatements() noexcept;
    void writeBatch(
        const std::deque<LiveMirrorOp>& rawBatch,
        uint32_t& activeFileId,
        bool& hasActiveFileId);
    void runCheckpointOnLiveDb();
    void logMirrorFlush(size_t rawOps, size_t coalescedOps, int64_t elapsedMs);

    void enqueueOp(LiveMirrorOp op);
    void rethrowWriterErrorLocked() const;

    sqlite3* db_live_{nullptr};

    sqlite3_stmt* stInsWord_{nullptr};
    sqlite3_stmt* stUpsertDoc_{nullptr};
    sqlite3_stmt* stDelPostings_{nullptr};
    sqlite3_stmt* stInsPosting_{nullptr};
    sqlite3_stmt* stMarkDeleted_{nullptr};

    std::thread writerThread_;
    std::deque<LiveMirrorOp> queue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable flushDoneCv_;

    bool stop_{false};
    bool flushNow_{false};
    bool requestCheckpoint_{false};
    bool writerActive_{false};
    bool writerStarted_{false};
    std::exception_ptr writerError_;

    static constexpr int kSchemaVersion = 4;
};

} // namespace inverted_index
