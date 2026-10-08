#pragma once

#include <optional>
#include <string_view>

namespace inverted_index {

enum class DocumentCatalogStorage {
    Memory,
    SQLite
};

[[nodiscard]] constexpr std::string_view toString(
    DocumentCatalogStorage storage) noexcept
{
    switch (storage) {
    case DocumentCatalogStorage::Memory:
        return "memory";
    case DocumentCatalogStorage::SQLite:
        return "sqlite";
    }
    return "memory";
}

[[nodiscard]] constexpr std::optional<DocumentCatalogStorage>
parseDocumentCatalogStorage(std::string_view value) noexcept
{
    if (value == "memory")
        return DocumentCatalogStorage::Memory;
    if (value == "sqlite")
        return DocumentCatalogStorage::SQLite;
    return std::nullopt;
}

} // namespace inverted_index
