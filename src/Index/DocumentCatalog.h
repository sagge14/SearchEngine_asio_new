#pragma once

#include "DocPaths.h"
#include "DocumentCatalogStorage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace inverted_index {

struct DocumentMetadata {
    int64_t mtimeTicks{};
    uint64_t size{};
};

struct DocumentRecord {
    uint32_t id{};
    std::wstring path;
    DocumentMetadata metadata;
    bool deleted{};
};

struct CatalogScanItem {
    uint32_t id{};
    std::size_t pathIndex{};
    DocumentMetadata metadata;
};

struct CatalogDiff {
    std::vector<CatalogScanItem> added;
    std::vector<CatalogScanItem> updated;
    std::vector<uint32_t> removed;
};

struct CatalogUpsertResult {
    DocumentRecord record;
    bool changed{};
};

class DocumentCatalog {
public:
    using RowVisitor = std::function<void(const DocumentRecord&)>;

    virtual ~DocumentCatalog() = default;

    [[nodiscard]] virtual DocumentCatalogStorage storage() const noexcept = 0;
    [[nodiscard]] virtual bool pathsLoadedInMemory() const noexcept = 0;
    [[nodiscard]] virtual std::size_t cacheCapacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] bool empty() const { return size() == 0; }

    virtual CatalogDiff scan(const std::vector<std::wstring>& paths) = 0;
    virtual CatalogUpsertResult upsert(
        const std::wstring& path,
        std::filesystem::file_time_type mtime,
        uint64_t size) = 0;
    [[nodiscard]] virtual std::optional<DocumentRecord> findByPath(
        const std::wstring& path) const = 0;
    [[nodiscard]] virtual std::vector<std::optional<DocumentRecord>> findByIds(
        const std::vector<uint32_t>& ids) const = 0;
    virtual void markRemoved(uint32_t id) = 0;
    virtual void notePersisted(const std::wstring& path, uint32_t id) = 0;

    virtual void stageBatchSnapshot(
        const std::vector<std::wstring>& paths,
        const std::vector<DocumentMetadata>& metadata) = 0;
    virtual void commitStagedBatch() = 0;
    virtual void rollbackStagedBatch() noexcept = 0;

    virtual void visitRows(const RowVisitor& visitor) const = 0;
    virtual void loadRows(std::vector<DocPaths::RawRow>&& rows) = 0;
    virtual void shrinkToFit() = 0;
};

[[nodiscard]] std::unique_ptr<DocumentCatalog> makeDocumentCatalog(
    DocumentCatalogStorage storage,
    DocPaths& memoryPaths,
    const std::string& sqlitePath);

} // namespace inverted_index
