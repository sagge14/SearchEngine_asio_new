#include "DocumentCatalog.h"

#include "MyUtils/Encoding.h"
#include "SQLite/sqlite3.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <numeric>

namespace inverted_index {
namespace {

[[noreturn]] void throwSqliteCatalog(
    sqlite3* db,
    const std::string& stage,
    int rc)
{
    throw std::runtime_error(
        "SQLite document catalog " + stage + " failed: rc=" +
        std::to_string(rc) + " err=" +
        (db ? sqlite3_errmsg(db) : "no database handle"));
}

class SQLiteHandle final {
public:
    SQLiteHandle(const std::string& path, int flags)
    {
        const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "no handle";
            if (db_)
                sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(
                "SQLite document catalog cannot open '" + path +
                "': " + message);
        }
        sqlite3_busy_timeout(db_, 3000);
    }

    ~SQLiteHandle()
    {
        if (db_)
            sqlite3_close(db_);
    }

    SQLiteHandle(const SQLiteHandle&) = delete;
    SQLiteHandle& operator=(const SQLiteHandle&) = delete;

    [[nodiscard]] sqlite3* get() const noexcept { return db_; }

private:
    sqlite3* db_{};
};

class Statement final {
public:
    Statement(sqlite3* db, const std::string& sql)
        : db_(db)
    {
        const int rc = sqlite3_prepare_v2(
            db_, sql.c_str(), static_cast<int>(sql.size()), &statement_, nullptr);
        if (rc != SQLITE_OK)
            throwSqliteCatalog(db_, "prepare", rc);
    }

    ~Statement()
    {
        if (statement_)
            sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* db_{};
    sqlite3_stmt* statement_{};
};

void exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        if (error)
            sqlite3_free(error);
        throw std::runtime_error(
            "SQLite document catalog schema failed: " + message);
    }
}

[[nodiscard]] bool tableExists(sqlite3* db, const char* table)
{
    Statement statement(
        db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;");
    sqlite3_bind_text(statement.get(), 1, table, -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    throwSqliteCatalog(db, "inspect schema", rc);
}

[[nodiscard]] bool tableHasColumn(
    sqlite3* db,
    const char* table,
    const char* column)
{
    Statement statement(
        db, "PRAGMA table_info(" + std::string(table) + ");");
    while (true) {
        const int rc = sqlite3_step(statement.get());
        if (rc == SQLITE_DONE)
            return false;
        if (rc != SQLITE_ROW)
            throwSqliteCatalog(db, "inspect table columns", rc);
        const unsigned char* name = sqlite3_column_text(statement.get(), 1);
        if (name &&
            std::string(reinterpret_cast<const char*>(name)) == column)
        {
            return true;
        }
    }
}

void ensureDocumentSchema(sqlite3* db)
{
    exec(db, "PRAGMA journal_mode=WAL;");
    exec(db,
         "CREATE TABLE IF NOT EXISTS docs("
         "doc_id INTEGER PRIMARY KEY, "
         "path TEXT NOT NULL, "
         "mtime_ticks INTEGER NOT NULL, "
         "size_int64 INTEGER NOT NULL, "
         "deleted INTEGER NOT NULL DEFAULT 0);");
    if (!tableHasColumn(db, "docs", "deleted")) {
        exec(db,
             "ALTER TABLE docs ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0;");
    }

    {
        Statement duplicate(
            db,
            "SELECT path, COUNT(*) FROM docs "
            "GROUP BY path HAVING COUNT(*) > 1 LIMIT 1;");
        const int rc = sqlite3_step(duplicate.get());
        if (rc == SQLITE_ROW) {
            const unsigned char* path = sqlite3_column_text(duplicate.get(), 0);
            throw std::runtime_error(
                "SQLite document catalog contains duplicate path '" +
                std::string(path ? reinterpret_cast<const char*>(path) : "") +
                "'; automatic deletion is disabled");
        }
        if (rc != SQLITE_DONE)
            throwSqliteCatalog(db, "check duplicate paths", rc);
    }

    exec(db,
         "CREATE UNIQUE INDEX IF NOT EXISTS idx_docs_path_unique "
         "ON docs(path);");
    exec(db,
         "CREATE INDEX IF NOT EXISTS idx_docs_deleted ON docs(deleted);");

    if (tableExists(db, "postings")) {
        Statement orphan(
            db,
            "SELECT p.doc_id FROM postings p "
            "LEFT JOIN docs d ON d.doc_id=p.doc_id "
            "WHERE d.doc_id IS NULL LIMIT 1;");
        const int rc = sqlite3_step(orphan.get());
        if (rc == SQLITE_ROW) {
            throw std::runtime_error(
                "SQLite document catalog is inconsistent with postings: "
                "missing docs row for doc_id=" +
                std::to_string(sqlite3_column_int64(orphan.get(), 0)));
        }
        if (rc != SQLITE_DONE)
            throwSqliteCatalog(db, "validate postings references", rc);
    }
}

[[nodiscard]] DocumentRecord readRecord(sqlite3_stmt* statement)
{
    const sqlite3_int64 id = sqlite3_column_int64(statement, 0);
    if (id < 0 || static_cast<uint64_t>(id) >
        std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error(
            "SQLite document catalog doc_id is outside uint32_t range");
    }
    const unsigned char* path = sqlite3_column_text(statement, 1);
    DocumentRecord record;
    record.id = static_cast<uint32_t>(id);
    record.path = encoding::utf8_to_wstring(
        path ? reinterpret_cast<const char*>(path) : "");
    record.metadata.mtimeTicks =
        static_cast<int64_t>(sqlite3_column_int64(statement, 2));
    record.metadata.size =
        static_cast<uint64_t>(sqlite3_column_int64(statement, 3));
    record.deleted = sqlite3_column_int(statement, 4) != 0;
    return record;
}

class MemoryDocumentCatalog final : public DocumentCatalog {
public:
    explicit MemoryDocumentCatalog(DocPaths& paths)
        : paths_(paths)
    {
    }

    [[nodiscard]] DocumentCatalogStorage storage() const noexcept override
    {
        return DocumentCatalogStorage::Memory;
    }

    [[nodiscard]] bool pathsLoadedInMemory() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::size_t cacheCapacity() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t size() const override { return paths_.size(); }

    CatalogDiff scan(const std::vector<std::wstring>& paths) override
    {
        const UpdatePack update = paths_.getUpdate(paths);
        CatalogDiff result;
        result.removed = update.removed;

        std::unordered_map<uint32_t, std::size_t> changed;
        changed.reserve(update.added.size() + update.updated.size());
        for (uint32_t id : update.added)
            changed.emplace(id, 0);
        for (uint32_t id : update.updated)
            changed.emplace(id, 1);

        for (std::size_t index = 0; index < paths.size(); ++index) {
            uint32_t id = 0;
            if (!paths_.tryGetId(paths[index], id))
                continue;
            const auto change = changed.find(id);
            if (change == changed.end())
                continue;
            int64_t ticks = 0;
            uint64_t size = 0;
            if (!paths_.getInfo(id, ticks, size))
                throw std::runtime_error("memory document catalog lost metadata");
            CatalogScanItem item{id, index, {ticks, size}};
            if (change->second == 0)
                result.added.push_back(item);
            else
                result.updated.push_back(item);
        }
        return result;
    }

    CatalogUpsertResult upsert(
        const std::wstring& path,
        std::filesystem::file_time_type mtime,
        uint64_t size) override
    {
        const auto [id, changed] = paths_.upsert(path, mtime, size);
        int64_t ticks = 0;
        uint64_t storedSize = 0;
        if (!paths_.getInfo(id, ticks, storedSize))
            throw std::runtime_error("memory document catalog lost metadata");
        return {{id, path, {ticks, storedSize}, paths_.isDeleted(id)}, changed};
    }

    [[nodiscard]] std::optional<DocumentRecord> findByPath(
        const std::wstring& path) const override
    {
        uint32_t id = 0;
        if (!paths_.tryGetId(path, id))
            return std::nullopt;
        int64_t ticks = 0;
        uint64_t size = 0;
        if (!paths_.getInfo(id, ticks, size))
            return std::nullopt;
        return DocumentRecord{id, path, {ticks, size}, paths_.isDeleted(id)};
    }

    [[nodiscard]] std::vector<std::optional<DocumentRecord>> findByIds(
        const std::vector<uint32_t>& ids) const override
    {
        std::vector<std::optional<DocumentRecord>> result(ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index) {
            int64_t ticks = 0;
            uint64_t size = 0;
            if (!paths_.getInfo(ids[index], ticks, size))
                continue;
            result[index] = DocumentRecord{
                ids[index],
                paths_.pathById(ids[index]),
                {ticks, size},
                paths_.isDeleted(ids[index])};
        }
        return result;
    }

    void markRemoved(uint32_t id) override { paths_.markRemoved(id); }
    void notePersisted(const std::wstring&, uint32_t) override {}

    void stageBatchSnapshot(
        const std::vector<std::wstring>& paths,
        const std::vector<DocumentMetadata>& metadata) override
    {
        if (paths.size() != metadata.size())
            throw std::runtime_error("batch document metadata size mismatch");
        rollback_ = paths_;
        std::vector<DocPaths::RawRow> rows;
        rows.reserve(paths.size());
        for (std::size_t index = 0; index < paths.size(); ++index) {
            rows.push_back(DocPaths::RawRow{
                static_cast<uint32_t>(index),
                paths[index],
                metadata[index].mtimeTicks,
                metadata[index].size,
                false});
        }
        paths_.rebuildFromRows(std::move(rows));
    }

    void commitStagedBatch() override { rollback_.reset(); }

    void rollbackStagedBatch() noexcept override
    {
        if (rollback_) {
            paths_ = std::move(*rollback_);
            rollback_.reset();
        }
    }

    void visitRows(const RowVisitor& visitor) const override
    {
        paths_.forEachRow(
            [&](uint32_t id,
                const std::wstring& path,
                int64_t ticks,
                uint64_t size,
                bool deleted) {
                visitor(DocumentRecord{id, path, {ticks, size}, deleted});
            });
    }

    void loadRows(std::vector<DocPaths::RawRow>&& rows) override
    {
        paths_.rebuildFromRows(std::move(rows));
    }

    void shrinkToFit() override { paths_.shrinkToFit(); }

private:
    DocPaths& paths_;
    std::optional<DocPaths> rollback_;
};

class SQLiteDocumentCatalog final : public DocumentCatalog {
public:
    explicit SQLiteDocumentCatalog(std::string path)
        : path_(std::move(path))
    {
        if (path_.empty())
            throw std::runtime_error("SQLite document catalog path is empty");
        SQLiteHandle db(
            path_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
        ensureDocumentSchema(db.get());
        refreshCounters(db.get());
    }

    [[nodiscard]] DocumentCatalogStorage storage() const noexcept override
    {
        return DocumentCatalogStorage::SQLite;
    }

    [[nodiscard]] bool pathsLoadedInMemory() const noexcept override
    {
        return false;
    }

    [[nodiscard]] std::size_t cacheCapacity() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t size() const override
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return documentCount_;
    }

    CatalogDiff scan(const std::vector<std::wstring>& paths) override
    {
        SQLiteHandle db(
            path_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        CatalogDiff result;
        // Detect duplicate scanner input using one compact index vector. Do
        // not retain a hash table (or any path copies) for the whole catalog.
        std::vector<std::size_t> pathOrder(paths.size());
        std::iota(pathOrder.begin(), pathOrder.end(), 0);
        std::sort(
            pathOrder.begin(), pathOrder.end(),
            [&paths](std::size_t left, std::size_t right) {
                return paths[left] < paths[right];
            });
        for (std::size_t index = 1; index < pathOrder.size(); ++index) {
            if (paths[pathOrder[index - 1]] == paths[pathOrder[index]]) {
                throw std::runtime_error(
                    "document scan contains duplicate path: " +
                    encoding::wstring_to_utf8(paths[pathOrder[index]]));
            }
        }

        uint32_t nextId = 0;
        std::size_t knownCount = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            nextId = nextId_;
            knownCount = documentCount_;
        }
        std::vector<uint8_t> seen(
            std::max<std::size_t>(knownCount, static_cast<std::size_t>(nextId)),
            0);

        const int variableLimit = std::max(
            1, sqlite3_limit(db.get(), SQLITE_LIMIT_VARIABLE_NUMBER, -1));
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min(variableLimit, 900));

        for (std::size_t begin = 0; begin < paths.size(); begin += chunkSize) {
            const std::size_t end = std::min(paths.size(), begin + chunkSize);
            std::string sql =
                "SELECT doc_id,path,mtime_ticks,size_int64,deleted "
                "FROM docs WHERE path IN (";
            for (std::size_t index = begin; index < end; ++index) {
                if (index != begin)
                    sql += ',';
                sql += '?';
            }
            sql += ");";

            Statement statement(db.get(), sql);
            std::vector<std::string> utf8Paths;
            utf8Paths.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                utf8Paths.push_back(encoding::wstring_to_utf8(paths[index]));
                sqlite3_bind_text(
                    statement.get(),
                    static_cast<int>(index - begin + 1),
                    utf8Paths.back().c_str(),
                    -1,
                    SQLITE_TRANSIENT);
            }

            std::unordered_map<std::string, DocumentRecord> rows;
            rows.reserve(end - begin);
            while (true) {
                const int rc = sqlite3_step(statement.get());
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                    throwSqliteCatalog(db.get(), "scan path lookup", rc);
                DocumentRecord record = readRecord(statement.get());
                rows.emplace(
                    encoding::wstring_to_utf8(record.path), std::move(record));
            }

            for (std::size_t index = begin; index < end; ++index) {
                const auto found = rows.find(utf8Paths[index - begin]);
                const auto mtime = std::filesystem::last_write_time(paths[index]);
                const uint64_t size = std::filesystem::file_size(paths[index]);
                const DocumentMetadata metadata{
                    static_cast<int64_t>(mtime.time_since_epoch().count()), size};
                if (found == rows.end()) {
                    const uint32_t id = nextId++;
                    if (id >= seen.size())
                        seen.resize(static_cast<std::size_t>(id) + 1, 0);
                    seen[id] = 1;
                    result.added.push_back({id, index, metadata});
                    continue;
                }

                const DocumentRecord& record = found->second;
                if (record.id >= seen.size())
                    seen.resize(static_cast<std::size_t>(record.id) + 1, 0);
                seen[record.id] = 1;
                if (record.deleted ||
                    record.metadata.mtimeTicks != metadata.mtimeTicks ||
                    record.metadata.size != metadata.size)
                {
                    result.updated.push_back({record.id, index, metadata});
                }
            }
        }

        {
            Statement active(db.get(),
                "SELECT doc_id FROM docs WHERE deleted=0 ORDER BY doc_id;");
            while (true) {
                const int rc = sqlite3_step(active.get());
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                    throwSqliteCatalog(db.get(), "scan removed documents", rc);
                const sqlite3_int64 rawId = sqlite3_column_int64(active.get(), 0);
                if (rawId < 0 || static_cast<uint64_t>(rawId) >
                    std::numeric_limits<uint32_t>::max())
                {
                    throw std::runtime_error(
                        "SQLite document catalog doc_id is outside uint32_t range");
                }
                const uint32_t id = static_cast<uint32_t>(rawId);
                if (id >= seen.size() || seen[id] == 0)
                    result.removed.push_back(id);
            }
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            nextId_ = nextId;
            documentCount_ += result.added.size();
        }
        return result;
    }

    CatalogUpsertResult upsert(
        const std::wstring& path,
        std::filesystem::file_time_type mtime,
        uint64_t size) override
    {
        const DocumentMetadata metadata{
            static_cast<int64_t>(mtime.time_since_epoch().count()), size};
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto pending = pending_.find(path);
            if (pending != pending_.end()) {
                DocumentRecord record = pending->second;
                const bool changed = record.deleted ||
                    record.metadata.mtimeTicks != metadata.mtimeTicks ||
                    record.metadata.size != metadata.size;
                record.metadata = metadata;
                record.deleted = false;
                pending->second = record;
                return {std::move(record), changed};
            }
        }

        if (auto existing = findPersistedByPath(path)) {
            const bool changed = existing->deleted ||
                existing->metadata.mtimeTicks != metadata.mtimeTicks ||
                existing->metadata.size != metadata.size;
            existing->metadata = metadata;
            existing->deleted = false;
            if (changed) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                pending_[path] = *existing;
            }
            return {std::move(*existing), changed};
        }

        std::lock_guard<std::mutex> lock(stateMutex_);
        DocumentRecord record{nextId_++, path, metadata, false};
        pending_[path] = record;
        ++documentCount_;
        return {std::move(record), true};
    }

    [[nodiscard]] std::optional<DocumentRecord> findByPath(
        const std::wstring& path) const override
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto pending = pending_.find(path);
            if (pending != pending_.end())
                return pending->second;
        }
        return findPersistedByPath(path);
    }

    [[nodiscard]] std::vector<std::optional<DocumentRecord>> findByIds(
        const std::vector<uint32_t>& ids) const override
    {
        std::vector<std::optional<DocumentRecord>> result(ids.size());
        if (ids.empty())
            return result;

        SQLiteHandle db(path_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        const int variableLimit = std::max(
            1, sqlite3_limit(db.get(), SQLITE_LIMIT_VARIABLE_NUMBER, -1));
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min(variableLimit, 900));

        std::unordered_map<uint32_t, std::vector<std::size_t>> positions;
        positions.reserve(ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index)
            positions[ids[index]].push_back(index);

        for (std::size_t begin = 0; begin < ids.size(); begin += chunkSize) {
            const std::size_t end = std::min(ids.size(), begin + chunkSize);
            std::string sql =
                "SELECT doc_id,path,mtime_ticks,size_int64,deleted "
                "FROM docs WHERE doc_id IN (";
            for (std::size_t index = begin; index < end; ++index) {
                if (index != begin)
                    sql += ',';
                sql += '?';
            }
            sql += ");";
            Statement statement(db.get(), sql);
            for (std::size_t index = begin; index < end; ++index) {
                sqlite3_bind_int64(
                    statement.get(),
                    static_cast<int>(index - begin + 1),
                    static_cast<sqlite3_int64>(ids[index]));
            }
            while (true) {
                const int rc = sqlite3_step(statement.get());
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                    throwSqliteCatalog(db.get(), "batch fetch by doc_id", rc);
                DocumentRecord record = readRecord(statement.get());
                const auto found = positions.find(record.id);
                if (found == positions.end())
                    continue;
                for (std::size_t position : found->second)
                    result[position] = record;
            }
        }
        return result;
    }

    void markRemoved(uint32_t) override {}

    void notePersisted(const std::wstring& path, uint32_t id) override
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto found = pending_.find(path);
        if (found != pending_.end() && found->second.id == id)
            pending_.erase(found);
    }

    void stageBatchSnapshot(
        const std::vector<std::wstring>& paths,
        const std::vector<DocumentMetadata>& metadata) override
    {
        if (paths.size() != metadata.size())
            throw std::runtime_error("batch document metadata size mismatch");
        std::lock_guard<std::mutex> lock(stateMutex_);
        stagedPaths_ = &paths;
        stagedMetadata_ = &metadata;
        stagedOldCount_ = documentCount_;
        stagedOldNextId_ = nextId_;
        documentCount_ = paths.size();
        nextId_ = static_cast<uint32_t>(paths.size());
    }

    void commitStagedBatch() override
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stagedPaths_ = nullptr;
        stagedMetadata_ = nullptr;
        stagedOldCount_.reset();
        stagedOldNextId_.reset();
    }

    void rollbackStagedBatch() noexcept override
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (stagedOldCount_)
            documentCount_ = *stagedOldCount_;
        if (stagedOldNextId_)
            nextId_ = *stagedOldNextId_;
        stagedPaths_ = nullptr;
        stagedMetadata_ = nullptr;
        stagedOldCount_.reset();
        stagedOldNextId_.reset();
    }

    void visitRows(const RowVisitor& visitor) const override
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (stagedPaths_ && stagedMetadata_) {
                for (std::size_t index = 0; index < stagedPaths_->size(); ++index) {
                    visitor(DocumentRecord{
                        static_cast<uint32_t>(index),
                        (*stagedPaths_)[index],
                        (*stagedMetadata_)[index],
                        false});
                }
                return;
            }
        }

        SQLiteHandle db(path_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        Statement statement(
            db.get(),
            "SELECT doc_id,path,mtime_ticks,size_int64,deleted "
            "FROM docs ORDER BY doc_id;");
        while (true) {
            const int rc = sqlite3_step(statement.get());
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW)
                throwSqliteCatalog(db.get(), "visit rows", rc);
            visitor(readRecord(statement.get()));
        }
    }

    void loadRows(std::vector<DocPaths::RawRow>&&) override
    {
        throw std::logic_error(
            "SQLite document catalog must not load all paths into memory");
    }

    void shrinkToFit() override
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pending_.rehash(0);
    }

private:
    [[nodiscard]] std::optional<DocumentRecord> findPersistedByPath(
        const std::wstring& path) const
    {
        SQLiteHandle db(path_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        Statement statement(
            db.get(),
            "SELECT doc_id,path,mtime_ticks,size_int64,deleted "
            "FROM docs WHERE path=?;");
        const std::string utf8Path = encoding::wstring_to_utf8(path);
        sqlite3_bind_text(
            statement.get(), 1, utf8Path.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        if (rc != SQLITE_ROW)
            throwSqliteCatalog(db.get(), "lookup by path", rc);
        return readRecord(statement.get());
    }

    void refreshCounters(sqlite3* db)
    {
        Statement statement(
            db,
            "SELECT COUNT(*), COALESCE(MAX(doc_id), -1) FROM docs;");
        const int rc = sqlite3_step(statement.get());
        if (rc != SQLITE_ROW)
            throwSqliteCatalog(db, "read counters", rc);
        const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
        const sqlite3_int64 maximum = sqlite3_column_int64(statement.get(), 1);
        if (count < 0 || maximum >=
            static_cast<sqlite3_int64>(std::numeric_limits<uint32_t>::max()))
        {
            throw std::runtime_error(
                "SQLite document catalog counters are outside supported range");
        }
        documentCount_ = static_cast<std::size_t>(count);
        nextId_ = maximum < 0 ? 0 : static_cast<uint32_t>(maximum + 1);
    }

    std::string path_;
    mutable std::mutex stateMutex_;
    std::size_t documentCount_{};
    uint32_t nextId_{};
    std::unordered_map<std::wstring, DocumentRecord> pending_;
    const std::vector<std::wstring>* stagedPaths_{};
    const std::vector<DocumentMetadata>* stagedMetadata_{};
    std::optional<std::size_t> stagedOldCount_;
    std::optional<uint32_t> stagedOldNextId_;
};

} // namespace

std::unique_ptr<DocumentCatalog> makeDocumentCatalog(
    DocumentCatalogStorage storage,
    DocPaths& memoryPaths,
    const std::string& sqlitePath)
{
    switch (storage) {
    case DocumentCatalogStorage::Memory:
        return std::make_unique<MemoryDocumentCatalog>(memoryPaths);
    case DocumentCatalogStorage::SQLite:
        return std::make_unique<SQLiteDocumentCatalog>(sqlitePath);
    }
    throw std::invalid_argument("unknown document catalog storage");
}

} // namespace inverted_index
