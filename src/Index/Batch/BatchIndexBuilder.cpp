#include "BatchIndexBuilder.h"

#include "BoundedByteQueue.h"
#include "Index/OEMFastTokenizer.h"
#include "Index/robin_hood.h"
#include "Index/simd_tokenizer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inverted_index::batch {
namespace {

using LocalDictionary =
    robin_hood::unordered_map<std::string, std::vector<Posting>>;
using FrequencyMap = robin_hood::unordered_map<std::string, std::size_t>;

constexpr std::size_t kReadBlockSize = 64u * 1024u;

enum class BlockKind {
    Start,
    Data,
    Complete,
    Abort
};

struct FileBlock {
    BlockKind kind = BlockKind::Data;
    uint32_t fileId{};
    bool isLastChunk{};
    std::vector<char> data;
};

struct DocumentAccumulator {
    uint32_t fileId{};
    FrequencyMap frequencies;
    std::string carry;
};

struct TermRun {
    const std::string* word{};
    std::vector<Posting>* postings{};
};

class FileReadError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AvailableWorkers final {
public:
    explicit AvailableWorkers(std::size_t count)
    {
        for (std::size_t worker = 0; worker < count; ++worker)
            workers_.push_back(worker);
    }

    [[nodiscard]] std::optional<std::size_t> acquire()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return cancelled_ || !workers_.empty();
        });
        if (cancelled_)
            return std::nullopt;

        const std::size_t worker = workers_.front();
        workers_.pop_front();
        return worker;
    }

    void release(std::size_t worker) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_)
            return;
        workers_.push_back(worker);
        condition_.notify_one();
    }

    void cancel() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

private:
    std::deque<std::size_t> workers_;
    bool cancelled_{};
    std::mutex mutex_;
    std::condition_variable condition_;
};

void tokenizeBlock(const FileBlock& block, DocumentAccumulator& document)
{
    const simd_tokenizer::TokenCallback onToken =
        [&document](std::string_view token) {
            std::string word(token.data(), token.size());
            OEMFastTokenizer::normalizeToken(word);
            if (!word.empty())
                ++document.frequencies[word];
        };

    simd_tokenizer::tokenize_oem866_buffer(
        block.data.data(),
        block.data.size(),
        block.isLastChunk,
        document.carry,
        onToken);
}

void commitDocument(
    DocumentAccumulator& document,
    LocalDictionary& dictionary)
{
    for (auto& [word, count] : document.frequencies) {
        const auto boundedCount = static_cast<uint16_t>(
            std::min<std::size_t>(
                count,
                std::numeric_limits<uint16_t>::max()));
        dictionary[word].push_back(Posting{document.fileId, boundedCount});
    }
}

void joinAll(std::vector<std::thread>& threads) noexcept
{
    for (auto& thread : threads) {
        if (thread.joinable())
            thread.join();
    }
}

} // namespace

BatchIndexBuilder::BatchIndexBuilder(BatchIndexOptions options)
    : options_(options)
{
    options_.readerThreads = std::max<std::size_t>(1, options_.readerThreads);
    options_.indexerThreads = resolveIndexerThreads(options_.indexerThreads);
    options_.queueMemoryBytes = std::max(
        options_.indexerThreads,
        std::max<std::size_t>(1, options_.queueMemoryBytes));
}

std::size_t BatchIndexBuilder::resolveIndexerThreads(
    std::size_t configuredThreads) noexcept
{
    if (configuredThreads != 0)
        return configuredThreads;
    const unsigned int detected = std::thread::hardware_concurrency();
    return detected == 0 ? 1 : static_cast<std::size_t>(detected);
}

BatchIndexSnapshot BatchIndexBuilder::build(
    const std::vector<std::wstring>& paths) const
{
    BatchIndexSnapshot snapshot;
    if (paths.size() >
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
    {
        throw std::runtime_error("batch index has too many documents");
    }
    snapshot.documentMetadata.resize(paths.size());

    const std::size_t laneCapacity = std::max<std::size_t>(
        1,
        options_.queueMemoryBytes / options_.indexerThreads);
    const std::size_t payloadSize = std::min(
        kReadBlockSize,
        laneCapacity);

    using Lane = BoundedByteQueue<FileBlock>;
    std::vector<std::unique_ptr<Lane>> lanes;
    lanes.reserve(options_.indexerThreads);
    for (std::size_t worker = 0;
         worker < options_.indexerThreads;
         ++worker)
    {
        lanes.push_back(std::make_unique<Lane>(laneCapacity));
    }

    AvailableWorkers availableWorkers(options_.indexerThreads);
    std::vector<LocalDictionary> localDictionaries(options_.indexerThreads);
    std::vector<BatchIndexFileError> fileErrors;
    std::mutex errorMutex;
    std::exception_ptr fatalError;
    std::mutex fatalMutex;
    std::atomic<std::size_t> nextJob{0};
    std::atomic<std::size_t> readersRemaining{options_.readerThreads};
    std::atomic<std::size_t> indexedFiles{0};
    std::atomic<std::uint64_t> bytesRead{0};

    const auto closeLanes = [&lanes] {
        for (auto& lane : lanes)
            lane->close();
    };

    const auto cancelPipeline = [&lanes, &availableWorkers] {
        availableWorkers.cancel();
        for (auto& lane : lanes)
            lane->cancel();
    };

    const auto addFileError =
        [&fileErrors, &errorMutex](
            uint32_t fileId,
            const std::wstring& path,
            std::string message) {
            std::lock_guard<std::mutex> lock(errorMutex);
            fileErrors.push_back(BatchIndexFileError{
                fileId, path, std::move(message)});
        };

    const auto setFatal =
        [&fatalError, &fatalMutex, &cancelPipeline](std::exception_ptr error) {
            {
                std::lock_guard<std::mutex> lock(fatalMutex);
                if (!fatalError)
                    fatalError = error;
            }
            cancelPipeline();
        };

    std::vector<std::thread> indexers;
    std::vector<std::thread> readers;
    indexers.reserve(options_.indexerThreads);
    readers.reserve(options_.readerThreads);

    try {
        for (std::size_t worker = 0;
             worker < options_.indexerThreads;
             ++worker)
        {
            indexers.emplace_back([&, worker] {
                try {
                    std::optional<DocumentAccumulator> document;
                    while (auto block = lanes[worker]->pop()) {
                        switch (block->kind) {
                        case BlockKind::Start:
                            if (document)
                                throw std::runtime_error(
                                    "batch indexer received overlapping files");
                            document.emplace();
                            document->fileId = block->fileId;
                            break;

                        case BlockKind::Data:
                            if (!document ||
                                document->fileId != block->fileId)
                            {
                                throw std::runtime_error(
                                    "batch indexer received an orphan data block");
                            }
                            tokenizeBlock(*block, *document);
                            break;

                        case BlockKind::Complete:
                            if (!document ||
                                document->fileId != block->fileId)
                            {
                                throw std::runtime_error(
                                    "batch indexer received an orphan completion");
                            }
                            commitDocument(
                                *document,
                                localDictionaries[worker]);
                            document.reset();
                            indexedFiles.fetch_add(
                                1, std::memory_order_relaxed);
                            availableWorkers.release(worker);
                            break;

                        case BlockKind::Abort:
                            if (!document ||
                                document->fileId != block->fileId)
                            {
                                throw std::runtime_error(
                                    "batch indexer received an orphan abort");
                            }
                            document.reset();
                            availableWorkers.release(worker);
                            break;
                        }
                    }

                    if (document)
                        throw std::runtime_error(
                            "batch indexer stopped inside a document");
                }
                catch (...) {
                    setFatal(std::current_exception());
                }
            });
        }

        for (std::size_t reader = 0;
             reader < options_.readerThreads;
             ++reader)
        {
            readers.emplace_back([&] {
                try {
                    std::array<char, kReadBlockSize> readBuffer{};
                    while (true) {
                        const std::size_t jobIndex =
                            nextJob.fetch_add(1, std::memory_order_relaxed);
                        if (jobIndex >= paths.size())
                            break;

                        const uint32_t fileId =
                            static_cast<uint32_t>(jobIndex);
                        const std::wstring& path = paths[jobIndex];
                        try {
                            const auto mtime =
                                std::filesystem::last_write_time(path);
                            snapshot.documentMetadata[fileId] = {
                                static_cast<int64_t>(
                                    mtime.time_since_epoch().count()),
                                std::filesystem::file_size(path)};
                        }
                        catch (const std::exception& exception) {
                            addFileError(fileId, path, exception.what());
                            continue;
                        }
                        std::ifstream file(
                            std::filesystem::path(path),
                            std::ios::binary);
                        if (!file.is_open()) {
                            addFileError(fileId, path, "file not found");
                            continue;
                        }

                        const auto worker = availableWorkers.acquire();
                        if (!worker)
                            break;
                        Lane& lane = *lanes[*worker];
                        bool started = false;

                        try {
                            if (!lane.push(
                                    FileBlock{
                                        BlockKind::Start,
                                        fileId,
                                        false,
                                        {}},
                                    1))
                            {
                                break;
                            }
                            started = true;

                            bool cancelled = false;
                            while (true) {
                                file.read(
                                    readBuffer.data(),
                                    static_cast<std::streamsize>(
                                        readBuffer.size()));
                                const std::streamsize readCount = file.gcount();
                                if (readCount == 0) {
                                    if (!file.eof())
                                        throw FileReadError("file read failed");
                                    break;
                                }
                                if (!file && !file.eof())
                                    throw FileReadError("file read failed");

                                const std::size_t count =
                                    static_cast<std::size_t>(readCount);
                                const bool isLastRead = file.eof();
                                bytesRead.fetch_add(
                                    static_cast<std::uint64_t>(count),
                                    std::memory_order_relaxed);

                                for (std::size_t offset = 0;
                                     offset < count;
                                     offset += payloadSize)
                                {
                                    const std::size_t partSize = std::min(
                                        payloadSize,
                                        count - offset);
                                    FileBlock block;
                                    block.kind = BlockKind::Data;
                                    block.fileId = fileId;
                                    block.isLastChunk =
                                        isLastRead &&
                                        offset + partSize == count;
                                    block.data.assign(
                                        readBuffer.data() + offset,
                                        readBuffer.data() + offset + partSize);
                                    if (!lane.push(
                                            std::move(block),
                                            partSize))
                                    {
                                        cancelled = true;
                                        break;
                                    }
                                }

                                if (cancelled || isLastRead)
                                    break;
                            }

                            if (cancelled)
                                break;

                            if (!lane.push(
                                    FileBlock{
                                        BlockKind::Complete,
                                        fileId,
                                        false,
                                        {}},
                                    1))
                            {
                                break;
                            }
                        }
                        catch (const FileReadError& exception) {
                            addFileError(fileId, path, exception.what());
                            if (started &&
                                !lane.push(
                                    FileBlock{
                                        BlockKind::Abort,
                                        fileId,
                                        false,
                                        {}},
                                    1))
                            {
                                break;
                            }
                        }
                    }
                }
                catch (...) {
                    setFatal(std::current_exception());
                }

                if (readersRemaining.fetch_sub(
                        1, std::memory_order_acq_rel) == 1)
                {
                    closeLanes();
                }
            });
        }
    }
    catch (...) {
        setFatal(std::current_exception());
    }

    joinAll(readers);
    closeLanes();
    joinAll(indexers);

    if (fatalError)
        std::rethrow_exception(fatalError);

    snapshot.fileErrors = std::move(fileErrors);
    snapshot.indexedFiles = indexedFiles.load(std::memory_order_relaxed);
    snapshot.bytesRead = bytesRead.load(std::memory_order_relaxed);
    if (!snapshot.fileErrors.empty())
        return snapshot;

    std::vector<TermRun> termRuns;
    std::size_t termRunCount = 0;
    for (const auto& dictionary : localDictionaries)
        termRunCount += dictionary.size();
    termRuns.reserve(termRunCount);

    for (auto& dictionary : localDictionaries) {
        for (auto& [word, postings] : dictionary)
            termRuns.push_back(TermRun{&word, &postings});
    }

    std::sort(
        termRuns.begin(),
        termRuns.end(),
        [](const TermRun& left, const TermRun& right) {
            return *left.word < *right.word;
        });

    std::size_t uniqueTerms = 0;
    for (std::size_t index = 0; index < termRuns.size(); ++index) {
        if (index == 0 || *termRuns[index - 1].word != *termRuns[index].word)
            ++uniqueTerms;
    }
    snapshot.idToWord.reserve(uniqueTerms);
    snapshot.postings.reserve(uniqueTerms);
    snapshot.wordToId.reserve(uniqueTerms);
    snapshot.wordRefs.reserve(paths.size());

    std::size_t runBegin = 0;
    while (runBegin < termRuns.size()) {
        std::size_t runEnd = runBegin + 1;
        while (runEnd < termRuns.size() &&
               *termRuns[runBegin].word == *termRuns[runEnd].word)
        {
            ++runEnd;
        }

        std::size_t postingCount = 0;
        for (std::size_t run = runBegin; run < runEnd; ++run)
            postingCount += termRuns[run].postings->size();

        std::vector<Posting> mergedPostings;
        mergedPostings.reserve(postingCount);
        for (std::size_t run = runBegin; run < runEnd; ++run) {
            auto& source = *termRuns[run].postings;
            mergedPostings.insert(
                mergedPostings.end(),
                std::make_move_iterator(source.begin()),
                std::make_move_iterator(source.end()));
            source.clear();
            source.shrink_to_fit();
        }

        std::sort(
            mergedPostings.begin(),
            mergedPostings.end(),
            [](const Posting& left, const Posting& right) {
                return left.fileId < right.fileId;
            });

        if (snapshot.idToWord.size() >=
            static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
        {
            throw std::runtime_error("batch index has too many unique words");
        }
        const uint32_t wordId =
            static_cast<uint32_t>(snapshot.idToWord.size());
        snapshot.idToWord.push_back(*termRuns[runBegin].word);
        snapshot.wordToId.emplace(snapshot.idToWord.back(), wordId);
        snapshot.postings.emplace_back();
        PostingList& postingList = snapshot.postings.back();

        std::size_t readIndex = 0;
        std::size_t writeIndex = 0;
        while (readIndex < mergedPostings.size()) {
            const uint32_t fileId = mergedPostings[readIndex].fileId;
            uint32_t count = 0;
            do {
                count = std::min<uint32_t>(
                    std::numeric_limits<uint16_t>::max(),
                    count + mergedPostings[readIndex].cnt);
                ++readIndex;
            } while (readIndex < mergedPostings.size() &&
                     mergedPostings[readIndex].fileId == fileId);

            mergedPostings[writeIndex++] =
                Posting{fileId, static_cast<uint16_t>(count)};
            snapshot.wordRefs[fileId].push_back(wordId);
        }
        mergedPostings.resize(writeIndex);
        postingList.assignSortedUnique(std::move(mergedPostings));

        runBegin = runEnd;
    }

    return snapshot;
}

} // namespace inverted_index::batch
