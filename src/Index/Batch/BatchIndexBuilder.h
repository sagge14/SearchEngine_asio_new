#pragma once

#include "Index/DocumentCatalog.h"
#include "Index/PostingList.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace inverted_index::batch {

struct BatchIndexOptions {
    std::size_t readerThreads = 1;
    std::size_t indexerThreads = 0;
    std::size_t queueMemoryBytes = 256u * 1024u * 1024u;
};

struct BatchIndexFileError {
    uint32_t fileId{};
    std::wstring path;
    std::string message;
};

struct BatchIndexSnapshot {
    std::vector<DocumentMetadata> documentMetadata;
    std::unordered_map<std::string, uint32_t> wordToId;
    std::vector<std::string> idToWord;
    std::vector<PostingList> postings;
    std::unordered_map<std::size_t, std::vector<uint32_t>> wordRefs;
    std::vector<BatchIndexFileError> fileErrors;
    std::size_t indexedFiles{};
    std::uint64_t bytesRead{};
};

class BatchIndexBuilder final {
public:
    explicit BatchIndexBuilder(BatchIndexOptions options);

    [[nodiscard]] BatchIndexSnapshot build(
        const std::vector<std::wstring>& paths) const;

    [[nodiscard]] static std::size_t resolveIndexerThreads(
        std::size_t configuredThreads) noexcept;

private:
    BatchIndexOptions options_;
};

} // namespace inverted_index::batch
