#include "SQLiteIndexSerializer.h"

#include "InvertedIndex.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"

#include "SQLite/sqlite3.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace inverted_index {

static void throwSqlite(sqlite3* db, const std::string& where, int rc)
{
    const char* msg = db ? sqlite3_errmsg(db) : "no_db";
    throw std::runtime_error(where + " rc=" + std::to_string(rc) + " err=" + msg);
}

SQLiteIndexSerializer::SQLiteIndexSerializer(std::string path, LiveMirrorConfig config)
    : path_(std::move(path))
    , config_(std::move(config))
{
    if (config_.flushIntervalSec <= 0.0)
        config_.flushIntervalSec = 2.0;
    config_.loadThreads = std::clamp(config_.loadThreads, 1, 16);
}

SQLiteIndexSerializer::~SQLiteIndexSerializer()
{
    try {
        flushPending();
    } catch (...) {}
    stopWriter();
    close();
}

std::string SQLiteIndexSerializer::kind() const { return "sqlite"; }

bool SQLiteIndexSerializer::exists() const
{
    return !path_.empty() && std::filesystem::exists(path_);
}

void SQLiteIndexSerializer::open()
{
    if (db_)
        return;
    if (path_.empty())
        throw std::runtime_error("SQLiteIndexSerializer: empty path");

    const int rc = sqlite3_open(path_.c_str(), &db_);
    if (rc != SQLITE_OK)
    {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLiteIndexSerializer: failed to open: " + path_);
    }

    sqlite3_busy_timeout(db_, 3000);
    exec(db_, "PRAGMA foreign_keys=OFF;");
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_, "PRAGMA synchronous=NORMAL;");
    exec(db_, "PRAGMA temp_store=MEMORY;");
}

void SQLiteIndexSerializer::close() noexcept
{
    if (!db_)
        return;
    sqlite3_close(db_);
    db_ = nullptr;
}

void SQLiteIndexSerializer::exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string msg = err ? err : "";
        if (err) sqlite3_free(err);
        throw std::runtime_error(std::string("SQLite exec failed: ") + msg + " sql=" + sql);
    }
}

sqlite3_stmt* SQLiteIndexSerializer::prepare(sqlite3* db, const char* sql)
{
    sqlite3_stmt* st = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    if (rc != SQLITE_OK)
        throwSqlite(db, std::string("prepare: ") + sql, rc);
    return st;
}

void SQLiteIndexSerializer::finalize(sqlite3_stmt* st) noexcept
{
    if (st) sqlite3_finalize(st);
}

void SQLiteIndexSerializer::beginImmediate(sqlite3* db)
{
    exec(db, "BEGIN IMMEDIATE;");
}

void SQLiteIndexSerializer::commit(sqlite3* db)
{
    exec(db, "COMMIT;");
}

void SQLiteIndexSerializer::rollback(sqlite3* db) noexcept
{
    if (!db) return;
    char* err = nullptr;
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

void SQLiteIndexSerializer::initSchema(sqlite3* db)
{
    exec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);");
    exec(db, "CREATE TABLE IF NOT EXISTS words(word_id INTEGER PRIMARY KEY, word TEXT NOT NULL);");
    exec(db, "CREATE TABLE IF NOT EXISTS docs(doc_id INTEGER PRIMARY KEY, path TEXT NOT NULL, mtime_ticks INTEGER NOT NULL, size_int64 INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);");
    exec(db, "CREATE TABLE IF NOT EXISTS postings(word_id INTEGER NOT NULL, doc_id INTEGER NOT NULL, cnt INTEGER NOT NULL, PRIMARY KEY(word_id, doc_id)) WITHOUT ROWID;");

    if (!tableHasColumn(db, "docs", "deleted"))
        exec(db, "ALTER TABLE docs ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0;");

    migratePostingsToWithoutRowid(db);

    exec(db, "CREATE INDEX IF NOT EXISTS idx_postings_doc ON postings(doc_id);");
    exec(db, "CREATE INDEX IF NOT EXISTS idx_docs_deleted ON docs(deleted);");

    {
        sqlite3_stmt* st = prepare(db,
            "INSERT INTO meta(key,value) VALUES('schema_version', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
        const std::string v = std::to_string(kSchemaVersion);
        sqlite3_bind_text(st, 1, v.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st);
        finalize(st);
        if (rc != SQLITE_DONE)
            throwSqlite(db, "set schema_version", rc);
    }
}

bool SQLiteIndexSerializer::tableHasColumn(sqlite3* db,
                                           const char* table,
                                           const char* column)
{
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    sqlite3_stmt* st = prepare(db, sql.c_str());
    bool found = false;
    while (true)
    {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
        {
            finalize(st);
            throwSqlite(db, "table_info", rc);
        }

        const unsigned char* name = sqlite3_column_text(st, 1);
        if (name && std::string(reinterpret_cast<const char*>(name)) == column)
        {
            found = true;
            break;
        }
    }
    finalize(st);
    return found;
}

std::string SQLiteIndexSerializer::tableSql(sqlite3* db, const char* table)
{
    sqlite3_stmt* st = prepare(
        db,
        "SELECT sql FROM sqlite_master WHERE type='table' AND name=?;");
    sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);

    std::string result;
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
    {
        const unsigned char* sql = sqlite3_column_text(st, 0);
        if (sql)
            result = reinterpret_cast<const char*>(sql);
    }
    else if (rc != SQLITE_DONE)
    {
        finalize(st);
        throwSqlite(db, "read table sql", rc);
    }

    finalize(st);
    return result;
}

void SQLiteIndexSerializer::migratePostingsToWithoutRowid(sqlite3* db)
{
    const std::string currentSql = tableSql(db, "postings");
    std::string normalizedSql = currentSql;
    std::transform(
        normalizedSql.begin(),
        normalizedSql.end(),
        normalizedSql.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    if (currentSql.empty() ||
        normalizedSql.find("WITHOUT ROWID") != std::string::npos)
    {
        return;
    }

    if (!sqlite3_get_autocommit(db))
        throw std::runtime_error(
            "SQLite postings migration requires autocommit mode");

    const auto t0 = std::chrono::steady_clock::now();
    LogFile::getIndex().write(
        "SQLITE_SCHEMA_MIGRATION_BEGIN from=2 to=3 table=postings");

    bool committed = false;
    beginImmediate(db);
    try
    {
        if (!tableSql(db, "postings_rowid_v2").empty())
            throw std::runtime_error(
                "SQLite migration: temporary table postings_rowid_v2 already exists");

        exec(db, "DROP INDEX IF EXISTS idx_postings_doc;");
        exec(db, "ALTER TABLE postings RENAME TO postings_rowid_v2;");
        exec(db,
             "CREATE TABLE postings("
             "word_id INTEGER NOT NULL, "
             "doc_id INTEGER NOT NULL, "
             "cnt INTEGER NOT NULL, "
             "PRIMARY KEY(word_id, doc_id)"
             ") WITHOUT ROWID;");
        exec(db,
             "INSERT INTO postings(word_id, doc_id, cnt) "
             "SELECT word_id, doc_id, cnt "
             "FROM postings_rowid_v2 "
             "ORDER BY word_id, doc_id;");
        exec(db, "DROP TABLE postings_rowid_v2;");
        exec(db, "CREATE INDEX idx_postings_doc ON postings(doc_id);");
        commit(db);
        committed = true;
    }
    catch (...)
    {
        if (!committed)
            rollback(db);
        throw;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LogFile::getIndex().write(
        "SQLITE_SCHEMA_MIGRATION_END from=2 to=3 elapsed_ms=" +
        std::to_string(elapsedMs));
}

void SQLiteIndexSerializer::logMirrorFlush(size_t rawOps, size_t coalescedOps, int64_t elapsedMs)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "SQLITE_MIRROR_FLUSH ops=" << rawOps
        << " coalesced=" << coalescedOps
        << " elapsed_ms=" << elapsedMs;
    LogFile::getIndex().write(oss.str());
}

void SQLiteIndexSerializer::openLiveDb()
{
    if (db_live_)
        return;
    if (path_.empty())
        throw std::runtime_error("SQLiteIndexSerializer: empty path");

    const int rc = sqlite3_open(path_.c_str(), &db_live_);
    if (rc != SQLITE_OK)
    {
        sqlite3_close(db_live_);
        db_live_ = nullptr;
        throw std::runtime_error("SQLiteIndexSerializer: failed to open live: " + path_);
    }

    sqlite3_busy_timeout(db_live_, 3000);
    exec(db_live_, "PRAGMA foreign_keys=OFF;");
    exec(db_live_, "PRAGMA journal_mode=WAL;");
    exec(db_live_, "PRAGMA synchronous=NORMAL;");
    exec(db_live_, "PRAGMA temp_store=MEMORY;");
    initSchema(db_live_);
    prepareLiveStatements();
}

void SQLiteIndexSerializer::closeLiveDb() noexcept
{
    finalizeLiveStatements();
    if (!db_live_)
        return;
    sqlite3_close(db_live_);
    db_live_ = nullptr;
}

void SQLiteIndexSerializer::prepareLiveStatements()
{
    if (stInsWord_) return;

    stInsWord_     = prepare(db_live_, "INSERT OR IGNORE INTO words(word_id, word) VALUES(?, ?);");
    stUpsertDoc_   = prepare(db_live_, "INSERT INTO docs(doc_id, path, mtime_ticks, size_int64, deleted) "
                                       "VALUES(?, ?, ?, ?, 0) "
                                       "ON CONFLICT(doc_id) DO UPDATE SET "
                                       "path=excluded.path, mtime_ticks=excluded.mtime_ticks, "
                                       "size_int64=excluded.size_int64, deleted=0;");
    stDelPostings_ = prepare(db_live_, "DELETE FROM postings WHERE doc_id=?;");
    stInsPosting_  = prepare(db_live_, "INSERT OR REPLACE INTO postings(word_id, doc_id, cnt) VALUES(?, ?, ?);");
    stMarkDeleted_ = prepare(db_live_, "UPDATE docs SET deleted=1 WHERE doc_id=?;");
}

void SQLiteIndexSerializer::finalizeLiveStatements() noexcept
{
    finalize(stInsWord_);     stInsWord_ = nullptr;
    finalize(stUpsertDoc_);   stUpsertDoc_ = nullptr;
    finalize(stDelPostings_); stDelPostings_ = nullptr;
    finalize(stInsPosting_);  stInsPosting_ = nullptr;
    finalize(stMarkDeleted_); stMarkDeleted_ = nullptr;
}

void SQLiteIndexSerializer::startWriter()
{
    if (writerStarted_)
        return;
    writerStarted_ = true;
    writerThread_ = std::thread([this] { writerLoop(); });
}

void SQLiteIndexSerializer::stopWriter()
{
    if (!writerStarted_)
        return;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stop_ = true;
        flushNow_ = true;
    }
    queueCv_.notify_all();

    if (writerThread_.joinable())
        writerThread_.join();

    writerStarted_ = false;
    stop_ = false;
}

void SQLiteIndexSerializer::writeBatch(const std::deque<LiveMirrorOp>& rawBatch)
{
    if (rawBatch.empty() || !db_live_)
        return;

    const auto t0 = std::chrono::steady_clock::now();

    std::unordered_map<uint32_t, LiveMirrorOp> coalesced;
    coalesced.reserve(rawBatch.size());
    for (const auto& op : rawBatch)
        coalesced[op.fileId] = op;

    std::unordered_map<uint32_t, std::string> allNewWords;
    for (const auto& [fid, op] : coalesced)
    {
        if (op.kind != LiveMirrorOp::Kind::Write)
            continue;
        for (const auto& [wid, word] : op.newWords)
            allNewWords.emplace(wid, word);
    }

    bool ok = false;
    beginImmediate(db_live_);
    try
    {
        for (const auto& [wid, word] : allNewWords)
        {
            sqlite3_reset(stInsWord_);
            sqlite3_clear_bindings(stInsWord_);
            sqlite3_bind_int(stInsWord_, 1, static_cast<int>(wid));
            sqlite3_bind_text(stInsWord_, 2, word.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stInsWord_);
            if (rc != SQLITE_DONE) throwSqlite(db_live_, "live insert word", rc);
        }

        for (const auto& [fileId, op] : coalesced)
        {
            if (op.kind == LiveMirrorOp::Kind::MarkDeleted)
            {
                sqlite3_reset(stMarkDeleted_);
                sqlite3_clear_bindings(stMarkDeleted_);
                sqlite3_bind_int(stMarkDeleted_, 1, static_cast<int>(fileId));
                const int rc = sqlite3_step(stMarkDeleted_);
                if (rc != SQLITE_DONE) throwSqlite(db_live_, "live mark deleted", rc);
                continue;
            }

            sqlite3_reset(stDelPostings_);
            sqlite3_clear_bindings(stDelPostings_);
            sqlite3_bind_int(stDelPostings_, 1, static_cast<int>(fileId));
            {
                const int rc = sqlite3_step(stDelPostings_);
                if (rc != SQLITE_DONE) throwSqlite(db_live_, "live delete old postings", rc);
            }

            for (const auto& [wid, cnt] : op.widCounts)
            {
                sqlite3_reset(stInsPosting_);
                sqlite3_clear_bindings(stInsPosting_);
                sqlite3_bind_int(stInsPosting_, 1, static_cast<int>(wid));
                sqlite3_bind_int(stInsPosting_, 2, static_cast<int>(fileId));
                sqlite3_bind_int(stInsPosting_, 3, static_cast<int>(cnt));
                const int rc = sqlite3_step(stInsPosting_);
                if (rc != SQLITE_DONE) throwSqlite(db_live_, "live insert posting", rc);
            }

            const std::string p8 = encoding::wstring_to_utf8(op.path);
            sqlite3_reset(stUpsertDoc_);
            sqlite3_clear_bindings(stUpsertDoc_);
            sqlite3_bind_int(stUpsertDoc_, 1, static_cast<int>(fileId));
            sqlite3_bind_text(stUpsertDoc_, 2, p8.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stUpsertDoc_, 3, static_cast<sqlite3_int64>(op.mtimeTicks));
            sqlite3_bind_int64(stUpsertDoc_, 4, static_cast<sqlite3_int64>(op.size));
            {
                const int rc = sqlite3_step(stUpsertDoc_);
                if (rc != SQLITE_DONE) throwSqlite(db_live_, "live upsert doc", rc);
            }
        }

        commit(db_live_);
        ok = true;
    }
    catch (...)
    {
        if (!ok) rollback(db_live_);
        throw;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    logMirrorFlush(rawBatch.size(), coalesced.size(), elapsedMs);
}

void SQLiteIndexSerializer::runCheckpointOnLiveDb()
{
    if (!db_live_) return;
    char* err = nullptr;
    sqlite3_exec(db_live_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

void SQLiteIndexSerializer::writerLoop()
{
    try {
        openLiveDb();
    } catch (const std::exception& e) {
        LogFile::getIndex().write(std::string("SQLITE_MIRROR_WRITER: openLiveDb EXCEPTION: ") + e.what());
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.clear();
            flushDoneCv_.notify_all();
        }
        return;
    } catch (...) {
        LogFile::getIndex().write("SQLITE_MIRROR_WRITER: openLiveDb unknown exception");
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.clear();
            flushDoneCv_.notify_all();
        }
        return;
    }

    const auto interval = std::chrono::duration<double>(config_.flushIntervalSec);

    while (true)
    {
        std::deque<LiveMirrorOp> batch;
        bool doCheckpoint = false;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (queue_.empty() && !flushNow_ && !requestCheckpoint_)
            {
                queueCv_.wait_for(lock, interval, [this] {
                    return stop_ || flushNow_ || requestCheckpoint_ || !queue_.empty();
                });
            }

            if (stop_ && queue_.empty() && !requestCheckpoint_)
                break;

            if (!queue_.empty())
                batch.swap(queue_);

            if (queue_.empty())
                flushNow_ = false;

            doCheckpoint = requestCheckpoint_;
        }

        if (!batch.empty())
        {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                writerActive_ = true;
            }
            try {
                writeBatch(batch);
            } catch (const std::exception& e) {
                LogFile::getIndex().write(std::string("SQLITE_MIRROR_WRITER: writeBatch EXCEPTION: ") + e.what());
            } catch (...) {
                LogFile::getIndex().write("SQLITE_MIRROR_WRITER: writeBatch unknown exception");
            }
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                writerActive_ = false;
                if (queue_.empty() && !requestCheckpoint_)
                    flushDoneCv_.notify_all();
            }
        }

        if (doCheckpoint)
        {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                writerActive_ = true;
            }
            try {
                runCheckpointOnLiveDb();
            } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                writerActive_ = false;
                requestCheckpoint_ = false;
                if (queue_.empty())
                    flushDoneCv_.notify_all();
            }
        }
        else if (batch.empty())
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (queue_.empty() && !writerActive_)
                flushDoneCv_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stop_ && queue_.empty() && !requestCheckpoint_)
                break;
        }
    }

    closeLiveDb();
}

void SQLiteIndexSerializer::enqueueOp(LiveMirrorOp op)
{
    if (!writerStarted_)
        startWriter();

    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back(std::move(op));
    if (config_.maxPendingOps > 0 &&
        queue_.size() >= static_cast<size_t>(config_.maxPendingOps))
    {
        flushNow_ = true;
    }
    queueCv_.notify_one();
}

void SQLiteIndexSerializer::openLive()
{
    startWriter();
}

void SQLiteIndexSerializer::writeFile(
        uint32_t fileId,
        const std::wstring& path,
        int64_t mtimeTicks,
        uint64_t size,
        const std::vector<std::pair<uint32_t, uint16_t>>& widCounts,
        const std::vector<std::pair<uint32_t, std::string>>& newWords,
        bool /*wasUpdate*/)
{
    LiveMirrorOp op;
    op.kind = LiveMirrorOp::Kind::Write;
    op.fileId = fileId;
    op.path = path;
    op.mtimeTicks = mtimeTicks;
    op.size = size;
    op.widCounts = widCounts;
    op.newWords = newWords;
    enqueueOp(std::move(op));
}

void SQLiteIndexSerializer::markFileDeleted(uint32_t fileId)
{
    LiveMirrorOp op;
    op.kind = LiveMirrorOp::Kind::MarkDeleted;
    op.fileId = fileId;
    enqueueOp(std::move(op));
}

void SQLiteIndexSerializer::flushPending()
{
    if (!writerStarted_)
        return;

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            flushNow_ = true;
            queueCv_.notify_all();
            flushDoneCv_.wait(lock, [this] {
                return queue_.empty() && !writerActive_;
            });
            if (queue_.empty() && !writerActive_)
                break;
        }
    }
}

void SQLiteIndexSerializer::checkpoint()
{
    if (!writerStarted_)
        return;

    flushPending();

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        requestCheckpoint_ = true;
        queueCv_.notify_all();
        flushDoneCv_.wait(lock, [this] {
            return !requestCheckpoint_ && !writerActive_;
        });
    }
}

void SQLiteIndexSerializer::save(const InvertedIndex& idx)
{
    flushPending();
    open();
    initSchema(db_);

    bool ok = false;
    beginImmediate(db_);
    sqlite3_stmt* insWord = nullptr;
    sqlite3_stmt* insDoc  = nullptr;
    sqlite3_stmt* insPost = nullptr;
    try
    {
        exec(db_, "DELETE FROM postings;");
        exec(db_, "DELETE FROM words;");
        exec(db_, "DELETE FROM docs;");

        InvertedIndex* nonConst = const_cast<InvertedIndex*>(&idx);

        insWord = prepare(db_, "INSERT INTO words(word_id, word) VALUES(?, ?);");
        auto id2word = idx.wordIds.exportId2Word();
        for (uint32_t wid = 0; wid < static_cast<uint32_t>(id2word.size()); ++wid)
        {
            sqlite3_reset(insWord);
            sqlite3_clear_bindings(insWord);

            sqlite3_bind_int(insWord, 1, static_cast<int>(wid));
            sqlite3_bind_text(insWord, 2, id2word[wid].c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(insWord);
            if (rc != SQLITE_DONE)
                throwSqlite(db_, "insert word", rc);
        }
        finalize(insWord);
        insWord = nullptr;

        insDoc = prepare(db_, "INSERT INTO docs(doc_id, path, mtime_ticks, size_int64, deleted) VALUES(?, ?, ?, ?, ?);");
        auto docRows = idx.docPaths.exportRows();
        for (const auto& r : docRows)
        {
            sqlite3_reset(insDoc);
            sqlite3_clear_bindings(insDoc);

            const std::string p8 = encoding::wstring_to_utf8(r.path);

            sqlite3_bind_int(insDoc, 1, static_cast<int>(r.id));
            sqlite3_bind_text(insDoc, 2, p8.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(insDoc, 3, static_cast<sqlite3_int64>(r.mtimeTicks));
            sqlite3_bind_int64(insDoc, 4, static_cast<sqlite3_int64>(r.fsize));
            sqlite3_bind_int(insDoc, 5, r.deleted ? 1 : 0);
            const int rc = sqlite3_step(insDoc);
            if (rc != SQLITE_DONE)
                throwSqlite(db_, "insert doc", rc);
        }
        finalize(insDoc);
        insDoc = nullptr;

        insPost = prepare(db_, "INSERT INTO postings(word_id, doc_id, cnt) VALUES(?, ?, ?);");
        {
            std::lock_guard<std::mutex> lock(nonConst->mapMutex);
            const size_t chunkCount = nonConst->dictionaryChunks.size();
            for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            {
                auto& up = nonConst->dictionaryChunks[chunkIndex];
                if (!up) continue;

                std::shared_lock<std::shared_mutex> lk(up->mutex);
                for (size_t local = 0; local < up->bucket.size(); ++local)
                {
                    const PostingList& pl = up->bucket[local];
                    if (pl.empty()) continue;

                    const uint32_t wid =
                        static_cast<uint32_t>(chunkIndex * InvertedIndex::CHUNK_SIZE + local);

                    for (const auto& p : pl)
                    {
                        sqlite3_reset(insPost);
                        sqlite3_clear_bindings(insPost);

                        sqlite3_bind_int(insPost, 1, static_cast<int>(wid));
                        sqlite3_bind_int(insPost, 2, static_cast<int>(p.fileId));
                        sqlite3_bind_int(insPost, 3, static_cast<int>(p.cnt));
                        const int rc = sqlite3_step(insPost);
                        if (rc != SQLITE_DONE)
                            throwSqlite(db_, "insert posting", rc);
                    }
                }
            }
        }
        finalize(insPost);
        insPost = nullptr;

        commit(db_);
        ok = true;
    }
    catch (...)
    {
        finalize(insWord);
        finalize(insDoc);
        finalize(insPost);
        if (!ok) rollback(db_);
        throw;
    }
}

void SQLiteIndexSerializer::load(InvertedIndex& idx)
{
    using Clock = std::chrono::steady_clock;
    const auto totalStart = Clock::now();
    auto stageStart = totalStart;

    const auto logStage = [&](const char* stage, size_t rows)
    {
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - stageStart).count();
        LogFile::getIndex().write(
            std::string("SQLITE_LOAD_STAGE stage=") + stage +
            " elapsed_ms=" + std::to_string(elapsedMs) +
            " rows=" + std::to_string(rows));
        stageStart = Clock::now();
    };

    bool readTransaction = false;
    size_t postingRows = 0;
    try
    {
        open();
        initSchema(db_);
        logStage("open_and_schema", 0);

        // Удерживаем write-reservation до конца восстановления. В WAL это
        // не мешает нашим read-only соединениям, но не позволяет внешнему
        // writer изменить слова/документы между параллельными диапазонами.
        beginImmediate(db_);
        readTransaction = true;

        std::vector<std::string> id2word;
        size_t wordRows = 0;
        {
            sqlite3_stmt* st = prepare(
                db_,
                "SELECT word_id, word FROM words ORDER BY word_id;");
            while (true)
            {
                const int rc = sqlite3_step(st);
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                {
                    finalize(st);
                    throwSqlite(db_, "select words", rc);
                }

                const sqlite3_int64 wid64 = sqlite3_column_int64(st, 0);
                if (wid64 < 0 ||
                    static_cast<uint64_t>(wid64) >
                        std::numeric_limits<uint32_t>::max())
                {
                    finalize(st);
                    throw std::runtime_error(
                        "SQLite load: word_id is outside uint32_t range");
                }

                const size_t wid = static_cast<size_t>(wid64);
                const unsigned char* w = sqlite3_column_text(st, 1);
                const std::string word =
                    w ? reinterpret_cast<const char*>(w) : "";

                if (wid >= id2word.size())
                    id2word.resize(wid + 1);
                id2word[wid] = word;
                ++wordRows;
            }
            finalize(st);
        }
        logStage("read_words", wordRows);

        std::unordered_map<std::string, uint32_t> word2id;
        word2id.reserve(id2word.size());
        for (uint32_t wid = 0;
             wid < static_cast<uint32_t>(id2word.size());
             ++wid)
        {
            word2id.emplace(id2word[wid], wid);
        }
        logStage("build_word_lookup", word2id.size());

        std::vector<DocPaths::RawRow> docs;
        {
            sqlite3_stmt* st = prepare(
                db_,
                "SELECT doc_id, path, mtime_ticks, size_int64, deleted "
                "FROM docs ORDER BY doc_id;");
            while (true)
            {
                const int rc = sqlite3_step(st);
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                {
                    finalize(st);
                    throwSqlite(db_, "select docs", rc);
                }

                const sqlite3_int64 did64 = sqlite3_column_int64(st, 0);
                if (did64 < 0 ||
                    static_cast<uint64_t>(did64) >
                        std::numeric_limits<uint32_t>::max())
                {
                    finalize(st);
                    throw std::runtime_error(
                        "SQLite load: doc_id is outside uint32_t range");
                }

                DocPaths::RawRow r;
                r.id = static_cast<uint32_t>(did64);
                const unsigned char* p = sqlite3_column_text(st, 1);
                const std::string p8 =
                    p ? reinterpret_cast<const char*>(p) : "";
                r.path = encoding::utf8_to_wstring(p8);
                r.mtimeTicks =
                    static_cast<int64_t>(sqlite3_column_int64(st, 2));
                r.fsize =
                    static_cast<uint64_t>(sqlite3_column_int64(st, 3));
                r.deleted = sqlite3_column_int(st, 4) != 0;
                docs.push_back(std::move(r));
            }
            finalize(st);
        }
        logStage("read_docs", docs.size());

        std::vector<size_t> postingCounts;
        size_t countedWords = 0;
        if (config_.precountPostings)
        {
            postingCounts.resize(id2word.size(), 0);
            sqlite3_stmt* st = prepare(
                db_,
                "SELECT word_id, COUNT(*) "
                "FROM postings GROUP BY word_id ORDER BY word_id;");
            while (true)
            {
                const int rc = sqlite3_step(st);
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                {
                    finalize(st);
                    throwSqlite(db_, "count postings by word", rc);
                }

                const sqlite3_int64 wid64 = sqlite3_column_int64(st, 0);
                const sqlite3_int64 count64 = sqlite3_column_int64(st, 1);
                if (wid64 < 0 ||
                    static_cast<uint64_t>(wid64) >= postingCounts.size() ||
                    count64 < 0 ||
                    static_cast<uint64_t>(count64) >
                        std::numeric_limits<size_t>::max())
                {
                    finalize(st);
                    throw std::runtime_error(
                        "SQLite load: invalid posting count row");
                }

                postingCounts[static_cast<size_t>(wid64)] =
                    static_cast<size_t>(count64);
                ++countedWords;
            }
            finalize(st);
        }

        std::vector<std::unique_ptr<InvertedIndex::Chunk>> newChunks;
        if (!id2word.empty())
        {
            const size_t chunkCount =
                (id2word.size() + InvertedIndex::CHUNK_SIZE - 1) /
                InvertedIndex::CHUNK_SIZE;
            newChunks.reserve(chunkCount);
            for (size_t i = 0; i < chunkCount; ++i)
                newChunks.push_back(
                    std::make_unique<InvertedIndex::Chunk>());

            if (config_.precountPostings)
            {
                for (size_t wid = 0; wid < postingCounts.size(); ++wid)
                {
                    if (postingCounts[wid] == 0)
                        continue;
                    const size_t chunkIndex =
                        wid / InvertedIndex::CHUNK_SIZE;
                    const size_t localIndex =
                        wid % InvertedIndex::CHUNK_SIZE;
                    newChunks[chunkIndex]->bucket[localIndex].reserve(
                        postingCounts[wid]);
                }
            }
        }
        LogFile::getIndex().write(
            "SQLITE_LOAD_RESERVE enabled=" +
            std::to_string(config_.precountPostings) +
            " counted_words=" + std::to_string(countedWords));
        logStage("count_and_reserve_postings", countedWords);

        decltype(idx.wordRefs) newWordRefs;
        newWordRefs.reserve(docs.size());

        const size_t requestedThreads =
            static_cast<size_t>(config_.loadThreads);
        const size_t loadThreads = newChunks.empty()
            ? 1
            : std::min(requestedThreads, newChunks.size());

        const auto loadPostingRange =
            [&](sqlite3* readDb,
                size_t wordBegin,
                size_t wordEnd,
                decltype(idx.wordRefs)* directWordRefs) -> size_t
        {
            sqlite3_stmt* st = nullptr;
            size_t rows = 0;
            try
            {
                st = prepare(
                    readDb,
                    "SELECT word_id, doc_id, cnt "
                    "FROM postings "
                    "WHERE word_id >= ? AND word_id < ? "
                    "ORDER BY word_id, doc_id;");
                sqlite3_bind_int64(
                    st, 1, static_cast<sqlite3_int64>(wordBegin));
                sqlite3_bind_int64(
                    st, 2, static_cast<sqlite3_int64>(wordEnd));

                while (true)
                {
                    const int rc = sqlite3_step(st);
                    if (rc == SQLITE_DONE)
                        break;
                    if (rc != SQLITE_ROW)
                        throwSqlite(readDb, "select postings range", rc);

                    const sqlite3_int64 wid64 =
                        sqlite3_column_int64(st, 0);
                    const sqlite3_int64 did64 =
                        sqlite3_column_int64(st, 1);
                    const sqlite3_int64 cnt64 =
                        sqlite3_column_int64(st, 2);
                    if (wid64 < 0 ||
                        static_cast<uint64_t>(wid64) >= id2word.size() ||
                        did64 < 0 ||
                        static_cast<uint64_t>(did64) >
                            std::numeric_limits<uint32_t>::max() ||
                        cnt64 < 0 ||
                        static_cast<uint64_t>(cnt64) >
                            std::numeric_limits<uint16_t>::max())
                    {
                        throw std::runtime_error(
                            "SQLite load: invalid posting row");
                    }

                    const uint32_t wid =
                        static_cast<uint32_t>(wid64);
                    const uint32_t did =
                        static_cast<uint32_t>(did64);
                    const size_t chunkIndex =
                        wid / InvertedIndex::CHUNK_SIZE;
                    const size_t localIndex =
                        wid % InvertedIndex::CHUNK_SIZE;

                    newChunks[chunkIndex]
                        ->bucket[localIndex]
                        .appendSorted(
                            did,
                            static_cast<uint16_t>(cnt64));
                    if (directWordRefs)
                        (*directWordRefs)[did].push_back(wid);
                    ++rows;
                }
                finalize(st);
                return rows;
            }
            catch (...)
            {
                finalize(st);
                throw;
            }
        };

        if (id2word.empty())
        {
            postingRows = 0;
        }
        else if (loadThreads == 1)
        {
            postingRows = loadPostingRange(
                db_, 0, id2word.size(), &newWordRefs);
        }
        else
        {
            std::vector<std::thread> readers;
            std::vector<size_t> rowsByThread(loadThreads, 0);
            std::vector<std::exception_ptr> errors(loadThreads);
            readers.reserve(loadThreads);

            for (size_t worker = 0; worker < loadThreads; ++worker)
            {
                const size_t chunkBegin =
                    newChunks.size() * worker / loadThreads;
                const size_t chunkEnd =
                    newChunks.size() * (worker + 1) / loadThreads;
                const size_t wordBegin =
                    chunkBegin * InvertedIndex::CHUNK_SIZE;
                const size_t wordEnd = std::min(
                    id2word.size(),
                    chunkEnd * InvertedIndex::CHUNK_SIZE);

                readers.emplace_back(
                    [&, worker, wordBegin, wordEnd]
                    {
                        sqlite3* readDb = nullptr;
                        try
                        {
                            const int rc = sqlite3_open_v2(
                                path_.c_str(),
                                &readDb,
                                SQLITE_OPEN_READONLY |
                                    SQLITE_OPEN_NOMUTEX,
                                nullptr);
                            if (rc != SQLITE_OK)
                                throwSqlite(
                                    readDb,
                                    "open parallel SQLite reader",
                                    rc);

                            sqlite3_busy_timeout(readDb, 3000);
                            exec(readDb, "BEGIN;");
                            rowsByThread[worker] = loadPostingRange(
                                readDb,
                                wordBegin,
                                wordEnd,
                                nullptr);
                            exec(readDb, "COMMIT;");
                            sqlite3_close(readDb);
                            readDb = nullptr;
                        }
                        catch (...)
                        {
                            if (readDb)
                            {
                                rollback(readDb);
                                sqlite3_close(readDb);
                            }
                            errors[worker] =
                                std::current_exception();
                        }
                    });
            }

            for (auto& reader : readers)
                reader.join();
            for (const auto& error : errors)
            {
                if (error)
                    std::rethrow_exception(error);
            }
            for (size_t rows : rowsByThread)
                postingRows += rows;

            // Параллельные читатели владеют непересекающимися чанками.
            // Обратные ссылки собираем после join, чтобы не синхронизировать
            // общий unordered_map на каждом постинге.
            for (size_t chunkIndex = 0;
                 chunkIndex < newChunks.size();
                 ++chunkIndex)
            {
                const auto& chunk = newChunks[chunkIndex];
                for (size_t localIndex = 0;
                     localIndex < chunk->bucket.size();
                     ++localIndex)
                {
                    const uint32_t wid = static_cast<uint32_t>(
                        chunkIndex * InvertedIndex::CHUNK_SIZE +
                        localIndex);
                    if (wid >= id2word.size())
                        break;

                    for (const Posting& posting :
                         chunk->bucket[localIndex])
                    {
                        newWordRefs[posting.fileId].push_back(wid);
                    }
                }
            }
        }

        LogFile::getIndex().write(
            "SQLITE_LOAD_POSTINGS threads=" +
            std::to_string(loadThreads) +
            " rows=" + std::to_string(postingRows));
        logStage("read_postings_and_build_refs", postingRows);

        exec(db_, "COMMIT;");
        readTransaction = false;

        {
            std::lock_guard<std::mutex> lock(idx.mapMutex);
            idx.wordIds.rebuild(std::move(word2id), std::move(id2word));
            idx.docPaths.rebuildFromRows(std::move(docs));
            idx.dictionary.clear();
            idx.dictionary.shrink_to_fit();
            idx.dictionaryChunks = std::move(newChunks);
            idx.wordRefs = std::move(newWordRefs);
        }
        logStage("install_index", postingRows);

        close();
    }
    catch (...)
    {
        if (readTransaction)
            rollback(db_);
        close();
        throw;
    }

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - totalStart).count();
    const double rowsPerSec =
        elapsedMs > 0
            ? static_cast<double>(postingRows) * 1000.0 /
                  static_cast<double>(elapsedMs)
            : 0.0;

    std::ostringstream totalLog;
    totalLog.imbue(std::locale::classic());
    totalLog << "SQLITE_LOAD_TOTAL elapsed_ms=" << elapsedMs
             << " postings=" << postingRows
             << " postings_per_sec=" << rowsPerSec;
    LogFile::getIndex().write(totalLog.str());
}

} // namespace inverted_index
