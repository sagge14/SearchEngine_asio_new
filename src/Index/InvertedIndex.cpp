//
// Created by user on 01.02.2023.
//

#include <boost/asio/post.hpp>
#include <windows.h>
#include <cassert>
#include <iostream>
#include <ranges>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <climits>
#include "InvertedIndex.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include <boost/algorithm/string.hpp>
#include "simd_tokenizer.h"
#include "OEMFastTokenizer.h"
#include <future>
#include <psapi.h>
#include "BoostIndexSerializer.h"
#include "SQLiteIndexSerializer.h"
#include "Batch/BatchIndexBuilder.h"

static size_t process_memory()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));
    return pmc.WorkingSetSize;   // байты
}

static void logIndexError(const std::wstring& path, const std::string& msg)
{
    LogFile::getErrors().write(encoding::wstring_to_utf8(path) + " | " + msg);
}

namespace {
    /// RAII: захватывает слот семафора при maxParallelReaders > 0; иначе no-op.
    struct ReadSlotGuard {
        std::optional<std::counting_semaphore<>>& sem;
        bool taken = false;

        explicit ReadSlotGuard(std::optional<std::counting_semaphore<>>& s) : sem(s)
        {
            if (sem.has_value()) {
                sem->acquire();
                taken = true;
            }
        }

        ~ReadSlotGuard()
        {
            if (taken)
                sem->release();
        }

        ReadSlotGuard(const ReadSlotGuard&) = delete;
        ReadSlotGuard& operator=(const ReadSlotGuard&) = delete;
        ReadSlotGuard(ReadSlotGuard&&) = delete;
        ReadSlotGuard& operator=(ReadSlotGuard&&) = delete;
    };
}

void inverted_index::InvertedIndex::pingIo(boost::asio::io_context& ctx, const char* name)
{
    inverted_index::InvertedIndex::addToLog(std::string("PING post -> ") + name);
    boost::asio::post(ctx, [name]{
        inverted_index::InvertedIndex::addToLog(std::string("PING executed <- ") + name);
    });
}


void inverted_index::InvertedIndex::commitSingleWord(const PostingTask& t)
{
    const uint32_t wid        = t.wordId;
    const size_t   chunkIndex = wid / CHUNK_SIZE;
    const size_t   localIndex = wid % CHUNK_SIZE;

    // Метаданные vector — под mapMutex; данные чанка — под chunk.mutex
    Chunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> g(mapMutex);
        if (chunkIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(chunkIndex + 1);
        if (!dictionaryChunks[chunkIndex])
            dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();
        chunkPtr = dictionaryChunks[chunkIndex].get();
    }

    std::unique_lock<std::shared_mutex> lk(chunkPtr->mutex);
    chunkPtr->bucket[localIndex][t.fileId] += t.count;
}

void inverted_index::InvertedIndex::commitChunkMap(
        std::unordered_map<size_t, std::vector<PostingTask>> chunkMap)
{
    // Только метаданные vector — под mapMutex; векторы PostingTask собираем после unlock (move).
    std::unordered_map<size_t, Chunk*> ptrByCid;
    ptrByCid.reserve(chunkMap.size());

    {
        std::lock_guard<std::mutex> g(mapMutex);

        size_t maxIndex = 0;
        for (const auto& [cid, _] : chunkMap)
            if (cid > maxIndex) maxIndex = cid;

        if (maxIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(maxIndex + 1);

        for (const auto& [cid, _] : chunkMap)
        {
            if (!dictionaryChunks[cid])
                dictionaryChunks[cid] = std::make_unique<Chunk>();
            ptrByCid[cid] = dictionaryChunks[cid].get();
        }
    }

    std::vector<std::pair<Chunk*, std::vector<PostingTask>>> work;
    work.reserve(chunkMap.size());
    for (auto& [cid, tasks] : chunkMap)
        work.emplace_back(ptrByCid[cid], std::move(tasks));

    for (auto& [ch, tasks] : work)
    {
        std::unique_lock<std::shared_mutex> lk(ch->mutex);

        for (auto& t : tasks)
        {
            size_t local = t.wordId % CHUNK_SIZE;
            ch->bucket[local][t.fileId] += t.count;
        }
    }
}


void inverted_index::InvertedIndex::processBatch(const PostingBatch& batch)
{
    try
    {
        if (batch.list.empty())
        {
            if (batch.promise)
                batch.promise->set_value();
            return;
        }

        // 1. LOCAL MAP: wordId → count
        robin_hood::unordered_map<uint32_t, uint32_t> widMap;
        widMap.reserve(batch.list.size());

        // Для live-зеркала: собираем новые слова (wid >= wordsBefore).
        const size_t wordsBefore = wordIds.size();
        std::vector<std::pair<uint32_t, std::string>> newWords;

        // 1.1. Преобразуем слова → wid + суммируем counts
        for (auto& [word, count] : batch.list)
        {
            uint32_t wid = wordIds.getId(word);
            widMap[wid] += count;

            if (wid >= wordsBefore)
                newWords.emplace_back(wid, word);

            // wordRefs обновляется здесь (в strand!)
            auto& refv = wordRefs[batch.fileId];
            if (refv.empty() || refv.back() != wid)
                refv.push_back(wid);
        }

        // 2. Группировка по chunkId
        std::unordered_map<size_t, std::vector<PostingTask>> chunkMap;
        chunkMap.reserve(widMap.size());

        for (auto& [wid, count] : widMap)
        {
            size_t chunkIndex = wid / CHUNK_SIZE;
            chunkMap[chunkIndex].push_back({ batch.fileId, wid, count });
        }

        // 3. Один commit-task в io_commit
        postTracked(cpu_pool_, [this,
                chunkMap = std::move(chunkMap),
                promise = batch.promise]()
        {
            try
            {
                commitChunkMap(std::move(chunkMap));
                if (promise) promise->set_value();
            }
            catch (const std::exception& e)
            {
                addToLog(std::string("io_commit handler exception: ") + e.what());
                if (promise) { try { promise->set_exception(std::current_exception()); } catch (...) {} }
            }
            catch (...)
            {
                addToLog("io_commit handler unknown exception");
                if (promise) { try { promise->set_exception(std::current_exception()); } catch (...) {} }
            }
        });

        // 4. Live-зеркало: ставим файл в очередь SQLite (не блокируем strand_).
        ensureSerializer();
        if (serializer_ && serializer_->supportsLiveUpdates())
        {
            std::vector<std::pair<uint32_t, uint16_t>> widCounts;
            widCounts.reserve(widMap.size());
            for (auto& [wid, count] : widMap)
                widCounts.emplace_back(
                    wid,
                    static_cast<uint16_t>(count > 0xFFFFu ? 0xFFFFu : count));

            int64_t mtimeTicks = 0;
            uint64_t fsize = 0;
            docPaths.getInfo(batch.fileId, mtimeTicks, fsize);
            std::wstring path = docPaths.pathById(batch.fileId);

            try {
                serializer_->writeFile(batch.fileId, path, mtimeTicks, fsize,
                                       widCounts, newWords, /*wasUpdate*/ true);
            }
            catch (const std::exception& e) {
                addToLog(std::string("processBatch: live enqueue EXCEPTION: ") + e.what());
            }
            catch (...) {
                addToLog("processBatch: live enqueue unknown exception");
            }
        }

    }
    catch (...)
    {
        if (batch.promise)
            batch.promise->set_exception(std::current_exception());
    }
}


// Внутренний безопасный помощник: копирует posting-лист слова
std::optional<PostingList> inverted_index::InvertedIndex::getPostingCopyByWord(const std::string& w) const
{
    std::lock_guard<std::mutex> stateLock(mapMutex);

    uint32_t wid;
    if (!wordIds.tryGet(w, wid))
        return std::nullopt;

    if (!dictionary.empty()) {
        if (wid >= dictionary.size() || dictionary[wid].empty())
            return std::nullopt;
        return dictionary[wid];
    }

    const size_t chunkIndex = wid / CHUNK_SIZE;
    const size_t localIndex = wid % CHUNK_SIZE;

    const Chunk* chunkPtr = nullptr;
    if (chunkIndex >= dictionaryChunks.size())
        return std::nullopt;
    const auto& up = dictionaryChunks[chunkIndex];
    if (!up)
        return std::nullopt;
    chunkPtr = up.get();

    std::shared_lock<std::shared_mutex> lk(chunkPtr->mutex);
    const PostingList& pl = chunkPtr->bucket[localIndex];
    if (pl.empty())
        return std::nullopt;

    return pl; // копия
}


void inverted_index::InvertedIndex::applyBatchInStrand(PostingBatch batch)
{
    // ВАЖНО: реальный commit выполняется строго в strand_,
    // чтобы все операции с dictionary/wordRefs были последовательны.
    postTracked(strand_, [this, batch = std::move(batch)]() mutable
    {
        try {

            processBatch(batch);
        }
        catch (const std::exception& e) {
            addToLog(std::string{"applyBatchInStrand: exception: "} + e.what());
        }
        catch (...) {
            addToLog("applyBatchInStrand: unknown exception");
        }
    });
}

void inverted_index::InvertedIndex::delFromDictionary(const std::vector<FileId>& list)
{
    postTracked(strand_, [this, list]() {
        for (FileId fileId : list)
            safeEraseFile(fileId);   // теперь безопасно, выполняется в commit-потоке
    });
}


void inverted_index::InvertedIndex::safeEraseFileInternal(FileId fileId)
{
    // Если есть wordRefs — идём только по нужным wid
    auto itRefs = wordRefs.find(fileId);
    if (itRefs != wordRefs.end())
    {
        const auto& widList = itRefs->second;
        std::vector<std::pair<Chunk*, size_t>> targets;
        targets.reserve(widList.size());
        {
            std::lock_guard<std::mutex> g(mapMutex);
            for (uint32_t wid : widList)
            {
                size_t chunkIndex = wid / CHUNK_SIZE;
                size_t localIndex = wid % CHUNK_SIZE;

                if (chunkIndex >= dictionaryChunks.size())
                    continue;

                auto& up = dictionaryChunks[chunkIndex];
                if (!up)
                    continue;

                targets.emplace_back(up.get(), localIndex);
            }
        }

        for (auto [chunk, localIndex] : targets)
        {
            std::unique_lock<std::shared_mutex> lk(chunk->mutex);
            auto& posting = chunk->bucket[localIndex];
            if (!posting.empty())
                posting.erase(fileId);
        }

        wordRefs.erase(itRefs);
    }
    else
    {
        // fallback: полный обход (редкий случай)
        std::vector<Chunk*> chunks;
        {
            std::lock_guard<std::mutex> g(mapMutex);
            chunks.reserve(dictionaryChunks.size());
            for (auto& up : dictionaryChunks)
                chunks.push_back(up.get());
        }
        for (Chunk* chunkPtr : chunks)
        {
            if (!chunkPtr) continue;

            std::unique_lock<std::shared_mutex> lk(chunkPtr->mutex);
            for (auto& posting : chunkPtr->bucket)
            {
                if (!posting.empty())
                    posting.erase(fileId);
            }
        }
    }

    addToLog("safeEraseFileInternal: removed fileId=" + std::to_string(fileId));
}





std::future<void> inverted_index::InvertedIndex::updateDocumentBase(
        const std::vector<std::wstring>& vecPaths)
{
    // The facade owns the complete maintenance boundary. Point operations
    // accepted before this lock are drained before strategy selection; point
    // operations arriving while it is held are coalesced for replay.
    std::unique_lock<std::mutex> updateLock(updateMutex);
    waitForIdle();

    std::future<void> result;

    try {
        if (storage_.fullIndexStrategy == FullIndexStrategy::Batch)
        {
            if (canUseBatchFullBuild())
                result = updateDocumentBaseBatch(vecPaths);
            else {
                addToLog(
                    "FULL_INDEX_FACADE strategy=batch action=legacy_incremental "
                    "reason=index_not_empty_after_drain");
                prepareLegacyMutableState();
                result = updateDocumentBaseLegacy(vecPaths);
            }
        }
        else {
            result = updateDocumentBaseLegacy(vecPaths);
        }
    }
    catch (...) {
        updateLock.unlock();
        drainDeferredPointPaths();
        throw;
    }

    updateLock.unlock();
    drainDeferredPointPaths();
    return result;
}

std::future<void> inverted_index::InvertedIndex::updateDocumentBaseLegacy(
        const std::vector<std::wstring>& vecPaths)
{
    const auto t0 = std::chrono::steady_clock::now();
    static std::atomic<uint64_t> s_sessionId{0};
    const uint64_t sessionId = ++s_sessionId;

    addToLog("INDEX_SESSION_BEGIN id=" + std::to_string(sessionId) +
             " full_index_strategy=legacy" +
             " max_parallel_readers=" + std::to_string(maxParallelReaders_) +
             " file_indexing_timeout_sec=" + std::to_string(fileIndexingTimeoutSec_));
    addToLog("updateDocumentBase() -> start");
    
    // Проверяем, не выполняется ли уже обновление
    bool expected = false;
    if (!work.compare_exchange_strong(expected, true)) {
        addToLog("updateDocumentBase() -> SKIP: update already in progress");
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    // RAII-обёртка для автоматического сброса work при выходе из функции
    struct WorkGuard {
        std::atomic<bool>& work_;
        explicit WorkGuard(std::atomic<bool>& w) : work_(w) {}
        ~WorkGuard() { work_.store(false, std::memory_order_release); }
    };
    WorkGuard workGuard(work);

    // 1. diff
    UpdatePack pack = docPaths.getUpdate(vecPaths);

    // ВАЖНО (вечный след): для исчезнувших файлов (pack.removed) постинги
    // НЕ стираем — лишь помечаем deleted. Стирание из памяти нужно только
    // для изменённых файлов (pack.updated), чтобы заменить старое содержимое.
    std::vector<FileId> toErase = pack.updated;
    std::vector<FileId> toMarkDeleted = pack.removed;

    std::vector<FileId> toIndex = pack.added;
    toIndex.insert(toIndex.end(), pack.updated.begin(), pack.updated.end());

    std::ostringstream diffLog;
    diffLog.imbue(std::locale::classic());
    diffLog << "diff: +"  << pack.added.size()
            << ", upd "   << pack.updated.size()
            << ", del "   << pack.removed.size();
    addToLog(diffLog.str());

    // Нет работы — сразу готовый future
    if (toErase.empty() && toMarkDeleted.empty() && toIndex.empty())
    {
        std::promise<void> p;
        p.set_value();
        return p.get_future();
        // work будет сброшен автоматически через WorkGuard
    }

    // Финальный promise
    auto finalPromise = std::make_shared<std::promise<void>>();
    std::future<void> future = finalPromise->get_future();

    try
    {
        // ------------------------------------------------------------------
        // 2. УДАЛЕНИЕ — строго в strand_, но синхронно
        // ------------------------------------------------------------------
        {
            std::promise<void> delPromise;
            auto delFuture = delPromise.get_future();

            postTracked(strand_, [this, &toErase, &toMarkDeleted, &delPromise]()
            {
                try {
                    // Изменённые файлы: стираем старые постинги в памяти.
                    for (FileId fileId : toErase)
                        safeEraseFileInternal(fileId);

                    // Исчезнувшие файлы: вечный след — постинги в памяти
                    // сохраняем, лишь помечаем deleted в зеркале SQLite.
                    ensureSerializer();
                    if (serializer_ && serializer_->supportsLiveUpdates())
                    {
                        for (FileId fileId : toMarkDeleted)
                        {
                            try { serializer_->markFileDeleted(fileId); }
                            catch (const std::exception& e) {
                                addToLog(std::string("updateDocumentBase: markFileDeleted EXCEPTION: ") + e.what());
                            }
                            catch (...) {
                                addToLog("updateDocumentBase: markFileDeleted unknown exception");
                            }
                        }
                    }

                    addToLog("updateDocumentBase: deletions done");
                    delPromise.set_value();
                }
                catch (...) {
                    delPromise.set_exception(std::current_exception());
                }
            });

            delFuture.get(); // ← ждём удаления
        }

        // ------------------------------------------------------------------
        // 3. ИНДЕКСАЦИЯ — CPU pool
        // ------------------------------------------------------------------
        std::vector<FileFuture> fileFutures;
        fileFutures.reserve(toIndex.size());

        // Lock-free счетчик прогресса
        std::atomic<size_t> processedFiles{0};
        const size_t totalFiles = toIndex.size();
        const size_t progressInterval = std::max<size_t>(100, totalFiles / 20); // логировать каждые 5% или 100 файлов

        for (FileId fileId : toIndex)
        {
            auto p = std::make_shared<std::promise<void>>();

            fileFutures.push_back({
                                          fileId,
                                          docPaths.pathById(fileId),
                                          p->get_future()
                                  });

            postTracked(cpu_pool_,
                              [this, fileId, p, &processedFiles, totalFiles, progressInterval]()
                              {
                                  try {
                                      fileIndexing(fileId, p);
                                      
                                      // Lock-free обновление счетчика
                                      size_t processed = processedFiles.fetch_add(1, std::memory_order_relaxed) + 1;
                                      
                                      // Логируем прогресс без блокировок (только при достижении интервала)
                                      if (processed % progressInterval == 0 || processed == totalFiles) {
                                          // Используем addToLog только для важных событий
                                          addToLog("Progress: " + std::to_string(processed) + "/" + 
                                                  std::to_string(totalFiles) + " files indexed (" +
                                                  std::to_string((processed * 100) / totalFiles) + "%)");
                                      }
                                  }
                                  catch (...) {
                                      try { p->set_exception(std::current_exception()); } catch (...) {}
                                  }
                              });
        }

        addToLog("updateDocumentBase: indexing tasks dispatched, files=" +
                 std::to_string(fileFutures.size()));

        // ------------------------------------------------------------------
        // 4. ОЖИДАНИЕ ВСЕХ fileIndexing (как было, но без detached)
        // ------------------------------------------------------------------
        const auto perFileTimeout = std::chrono::seconds(fileIndexingTimeoutSec_);
        for (auto& ff : fileFutures)
        {
            if (ff.fut.wait_for(perFileTimeout) != std::future_status::ready)
            {
                const std::string pathUtf8 = encoding::wstring_to_utf8(ff.path);
                addToLog("updateDocumentBase: TIMEOUT (" +
                         std::to_string(fileIndexingTimeoutSec_) +
                         "s) waiting file future, id=" +
                         std::to_string(ff.id) + " path=" + pathUtf8);
                logIndexError(ff.path,
                              "TIMEOUT waiting file future (" +
                              std::to_string(fileIndexingTimeoutSec_) + "s)");
                continue;
            }

            try { ff.fut.get(); }
            catch (const std::exception& e) { logIndexError(ff.path, e.what()); }
            catch (...) { logIndexError(ff.path, "unknown exception"); }
        }

        addToLog("updateDocumentBase: all fileIndexing done");

        // Пытаемся отдать обратно ёмкости векторов и страницы рабочего
        // набора, накопившиеся за время инкрементальных вставок.
        // Делать имеет смысл именно здесь — индексация закончена,
        // updateMutex удерживается, дальнейших массовых вставок не будет.
        {
            const size_t memBefore = process_memory();
            compactMemory();
            const size_t memAfter  = process_memory();

            std::ostringstream cm;
            cm.imbue(std::locale::classic());
            cm << "compactMemory: " << (memBefore / 1024 / 1024)
               << " MB -> " << (memAfter / 1024 / 1024) << " MB"
               << " (saved "
               << ((memBefore > memAfter) ? (memBefore - memAfter) / 1024 / 1024 : 0)
               << " MB)";
            addToLog(cm.str());
        }

        // Сохранение на диск — только по настройке save_dictionary_to_file в SearchServer::updateStep

        // Log dictionary statistics and memory usage
        auto stats = getStats();
        size_t memBytes = process_memory();
        size_t dict_size;
        {
            std::lock_guard<std::mutex> g(mapMutex);
            dict_size = dictionaryChunks.size() * CHUNK_SIZE;
        }
        double hole_percent = (dict_size > 0) ? (stats.emptyPostings * 100.0 / dict_size) : 0.0;
        
        std::ostringstream statsLog;
        statsLog.imbue(std::locale::classic());
        statsLog << "DICTIONARY STATS: unique_words=" << stats.uniqueWords
                 << ", total_postings=" << stats.totalPostings
                 << ", total_files=" << stats.totalFiles
                 << ", dictionary_slots=" << dict_size
                 << ", holes=" << stats.emptyPostings
                 << ", hole_percent=" << std::fixed << std::setprecision(2) << hole_percent << "%"
                 << ", dictionary_memory=" << (stats.memoryBytes / 1024 / 1024) << " MB"
                 << ", process_memory=" << (memBytes / 1024 / 1024) << " MB";
        addToLog(statsLog.str());

        ensureSerializer();
        if (serializer_ && serializer_->supportsLiveUpdates())
        {
            try {
                serializer_->flushPending();
                addToLog("updateDocumentBase: sqlite mirror flushPending done");
            }
            catch (const std::exception& e) {
                addToLog(std::string("updateDocumentBase: flushPending EXCEPTION: ") + e.what());
            }
            catch (...) {
                addToLog("updateDocumentBase: flushPending unknown exception");
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const double elapsedSec = (elapsedMs > 0) ? (elapsedMs / 1000.0) : 0.0;
        const double filesPerSec = (elapsedSec > 0.0) ? (static_cast<double>(totalFiles) / elapsedSec) : 0.0;
        addToLog("INDEX_SESSION_END id=" + std::to_string(sessionId) +
                 " files=" + std::to_string(totalFiles) +
                 " elapsed_ms=" + std::to_string(elapsedMs) +
                 " files_per_sec=" + std::to_string(filesPerSec));

        finalPromise->set_value();
        // work будет сброшен автоматически через WorkGuard
    }
    catch (...)
    {
        try { finalPromise->set_exception(std::current_exception()); } catch (...) {}
        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        addToLog("INDEX_SESSION_END id=" + std::to_string(sessionId) +
                 " status=exception elapsed_ms=" + std::to_string(elapsedMs));
        // work будет сброшен автоматически через WorkGuard
    }

    return future;
}

bool inverted_index::InvertedIndex::canUseBatchFullBuild() const
{
    std::lock_guard<std::mutex> lock(mapMutex);
    return docPaths.size() == 0 && wordIds.size() == 0 &&
           dictionaryChunks.empty() && dictionary.empty();
}

std::future<void> inverted_index::InvertedIndex::updateDocumentBaseBatch(
        const std::vector<std::wstring>& vecPaths)
{
    auto finalPromise = std::make_shared<std::promise<void>>();
    std::future<void> future = finalPromise->get_future();
    const auto startedAt = std::chrono::steady_clock::now();
    static std::atomic<uint64_t> s_batchSessionId{0};
    const uint64_t sessionId = ++s_batchSessionId;

    bool expected = false;
    if (!work.compare_exchange_strong(expected, true)) {
        addToLog("FULL_INDEX_FACADE strategy=batch action=skip reason=busy");
        finalPromise->set_value();
        return future;
    }

    struct WorkGuard {
        std::atomic<bool>& value;
        ~WorkGuard() { value.store(false, std::memory_order_release); }
    } workGuard{work};

    addToLog(
        "INDEX_SESSION_BEGIN id=batch-" + std::to_string(sessionId) +
        " full_index_strategy=batch reader_threads=" +
        std::to_string(storage_.batchReaderThreads) +
        " indexer_threads=" +
        std::to_string(batch::BatchIndexBuilder::resolveIndexerThreads(
            storage_.batchIndexerThreads)) +
        " queue_memory_bytes=" +
        std::to_string(storage_.batchQueueMemoryBytes));

    try {
        if (!batchBuilder_)
            throw std::runtime_error("batch index builder is not configured");

        batch::BatchIndexSnapshot snapshot = batchBuilder_->build(vecPaths);
        const std::size_t indexedFiles = snapshot.indexedFiles;
        const std::size_t failedFiles = snapshot.fileErrors.size();
        const std::uint64_t bytesRead = snapshot.bytesRead;
        const std::size_t uniqueWords = snapshot.idToWord.size();

        for (const auto& error : snapshot.fileErrors)
            logIndexError(error.path, error.message);

        if (!snapshot.fileErrors.empty()) {
            throw std::runtime_error(
                "batch full build failed to read " +
                std::to_string(snapshot.fileErrors.size()) +
                " file(s); snapshot was not published");
        }

        installBatchSnapshot(std::move(snapshot));

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        addToLog(
            "INDEX_SESSION_END id=batch-" + std::to_string(sessionId) +
            " full_index_strategy=batch indexed_files=" +
            std::to_string(indexedFiles) +
            " failed_files=" + std::to_string(failedFiles) +
            " unique_words=" + std::to_string(uniqueWords) +
            " bytes_read=" + std::to_string(bytesRead) +
            " elapsed_ms=" + std::to_string(elapsedMs));
        finalPromise->set_value();
    }
    catch (...) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        addToLog(
            "INDEX_SESSION_END id=batch-" + std::to_string(sessionId) +
            " full_index_strategy=batch status=exception elapsed_ms=" +
            std::to_string(elapsedMs));
        try { finalPromise->set_exception(std::current_exception()); }
        catch (...) {}
    }

    return future;
}

void inverted_index::InvertedIndex::installBatchSnapshot(
        batch::BatchIndexSnapshot&& snapshot)
{
    WordIdManager newWordIds;
    newWordIds.rebuild(
        std::move(snapshot.wordToId),
        std::move(snapshot.idToWord));

    std::vector<PostingList> newDictionary = std::move(snapshot.postings);

    WordIdManager oldWordIds;
    DocPaths oldDocPaths;
    std::vector<std::unique_ptr<Chunk>> oldChunks;
    decltype(wordRefs) oldWordRefs;
    std::vector<PostingList> oldDictionary;

    {
        std::lock_guard<std::mutex> lock(mapMutex);
        oldWordIds = std::move(wordIds);
        oldDocPaths = std::move(docPaths);
        oldChunks = std::move(dictionaryChunks);
        oldWordRefs = std::move(wordRefs);
        oldDictionary = std::move(dictionary);

        wordIds = std::move(newWordIds);
        docPaths = std::move(snapshot.documents);
        dictionaryChunks.clear();
        wordRefs = std::move(snapshot.wordRefs);
        dictionary = std::move(newDictionary);
    }

    try {
        saveFullSnapshot();
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(mapMutex);
        wordIds = std::move(oldWordIds);
        docPaths = std::move(oldDocPaths);
        dictionaryChunks = std::move(oldChunks);
        wordRefs = std::move(oldWordRefs);
        dictionary = std::move(oldDictionary);
        throw;
    }
}

void inverted_index::InvertedIndex::saveFullSnapshot()
{
    ensureSerializer();
    if (!serializer_)
        throw std::runtime_error("index serializer is not configured");

    if (serializer_->supportsLiveUpdates())
        serializer_->flushPending();
    else if (dictionary.empty()) {
        std::lock_guard<std::mutex> lock(mapMutex);
        rebuildDictionaryFromChunksLocked();
    }

    serializer_->save(*this);

    if (serializer_->supportsLiveUpdates()) {
        try { serializer_->checkpoint(); }
        catch (const std::exception& exception) {
            addToLog(
                std::string("batch snapshot checkpoint failed: ") +
                exception.what());
        }
        catch (...) {
            addToLog("batch snapshot checkpoint failed: unknown exception");
        }
    }
}

void inverted_index::InvertedIndex::prepareLegacyMutableState()
{
    std::lock_guard<std::mutex> lock(mapMutex);
    if (dictionary.empty())
        return;

    dictionaryChunks.clear();
    const std::size_t chunkCount =
        (dictionary.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    dictionaryChunks.reserve(chunkCount);
    for (std::size_t index = 0; index < chunkCount; ++index)
        dictionaryChunks.push_back(std::make_unique<Chunk>());

    for (std::size_t wordId = 0; wordId < dictionary.size(); ++wordId) {
        dictionaryChunks[wordId / CHUNK_SIZE]
            ->bucket[wordId % CHUNK_SIZE] = std::move(dictionary[wordId]);
    }
    dictionary.clear();
    dictionary.shrink_to_fit();
    addToLog("FULL_INDEX_FACADE batch snapshot converted for legacy mutation");
}


void logIndexingDebug(const std::string& msg) {
    LogFile::getIndex().write(msg);
}

void inverted_index::InvertedIndex::fileIndexing(
        FileId fileId,
        std::shared_ptr<std::promise<void>> promise)
{
    try
    {
        std::wstring fullPath = docPaths.pathById(fileId); // КОПИЯ!

        // --- словарь частот ---
        robin_hood::unordered_map<std::string, size_t> freqWordFile;

        // Чтение и токенизация — под слотом семафора (если maxParallelReaders > 0).
        // Слот удерживается только на время I/O+tokenize одного файла; формирование
        // batch и постинг в strand_ происходят уже без слота.
        {
            ReadSlotGuard slot(readSlots_);

            std::ifstream file(std::filesystem::path(fullPath), std::ios::binary);
            if (!file.is_open())
            {
                addToLog("fileIndexing: cannot open file " +
                         encoding::wstring_to_utf8(fullPath));

                promise->set_exception(
                        std::make_exception_ptr(
                                std::runtime_error("file not found")));
                return;
            }

            // --- SIMD tokenizer ---
            static const size_t BUF_SIZE = 64 * 1024;
            std::vector<char> buf(BUF_SIZE);

            std::string carry;   // хвост слова на границах буферов

            while (true)
            {
                file.read(buf.data(), BUF_SIZE);
                std::size_t readBytes = file.gcount();
                if (readBytes == 0)
                    break;

                const bool isLastChunk = file.eof();

                simd_tokenizer::tokenize_oem866_buffer(
                        buf.data(),
                        readBytes,
                        isLastChunk,
                        carry,
                        [&](std::string_view tok)
                        {
                            std::string w;
                            w.assign(tok.data(), tok.size());

                            OEMFastTokenizer::normalizeToken(w);

                            if (!w.empty())
                                ++freqWordFile[w];
                        });
            }
        } // slot освобождается здесь

        // создаём batch
        PostingBatch batch;
        batch.fileId = fileId;
        batch.promise = promise;

        batch.list.reserve(freqWordFile.size());
        for (auto &[word, count] : freqWordFile)
            batch.list.emplace_back(word, count);

        applyBatchInStrand(std::move(batch));
    }
    catch (...)
    {
        addToLog("fileIndexing: EXCEPTION for fileId=" + std::to_string(fileId));
        try { promise->set_exception(std::current_exception()); }
        catch (...) {}
    }
}


void inverted_index::InvertedIndex::addToLog(const string& _s) {
    LogFile::getIndex().write(_s);
}

PostingList inverted_index::InvertedIndex::getWordCount(const std::string& word)
{
    if (auto opt = getPostingCopyByWord(word))
        return *opt;
    return {};
}

void inverted_index::InvertedIndex::dictonaryToLog() const {
    // for(const auto& i:dictionary)
    //      addToLog(i.first + "\t" + std::to_string(i.second.size()));
}

inverted_index::DictionaryStats inverted_index::InvertedIndex::getStats() const
{
    DictionaryStats stats{};
    std::lock_guard<std::mutex> stateLock(mapMutex);
    
    // Реальное количество уникальных слов
    stats.uniqueWords = wordIds.size();
    stats.totalFiles = docPaths.size();
    
    // Подсчет постингов из chunks (без длительных блокировок)
    size_t totalPostings = 0;
    size_t emptyPostings = 0;
    size_t memoryBytes = 0;

    if (!dictionary.empty()) {
        for (const auto& posting : dictionary) {
            if (posting.empty()) {
                ++emptyPostings;
                continue;
            }
            totalPostings += posting.size();
            memoryBytes += posting.size() * 6 + 24;
        }
        stats.totalPostings = totalPostings;
        stats.emptyPostings = emptyPostings;
        stats.memoryBytes = memoryBytes;
        return stats;
    }

    for (const auto& chunkOwner : dictionaryChunks)
    {
        const Chunk* chunkPtr = chunkOwner.get();
        if (!chunkPtr) {
            // Пустой chunk = CHUNK_SIZE пустых постингов
            emptyPostings += CHUNK_SIZE;
            continue;
        }

        const Chunk& chunk = *chunkPtr;
        std::shared_lock<std::shared_mutex> lk(chunk.mutex);
        
        for (const auto& posting : chunk.bucket)
        {
            size_t postingSize = posting.size();
            if (postingSize == 0) {
                ++emptyPostings;
            } else {
                totalPostings += postingSize;
                // Posting = 6 bytes (uint32_t fileId + uint16_t cnt)
                // + overhead vector (~24 bytes на пустой вектор)
                memoryBytes += postingSize * 6 + 24;
            }
        }
    }
    
    stats.totalPostings = totalPostings;
    stats.emptyPostings = emptyPostings;
    stats.memoryBytes = memoryBytes;
    
    return stats;
}

void inverted_index::InvertedIndex::rebuildDictionaryFromChunksLocked()
{
    dictionary.clear();
    const size_t wordCount = wordIds.size();
    dictionary.reserve(wordCount);

    for (size_t wid = 0; wid < wordCount; ++wid)
    {
        const size_t chunkIndex = wid / CHUNK_SIZE;
        const size_t localIndex = wid % CHUNK_SIZE;
        if (chunkIndex >= dictionaryChunks.size()) {
            dictionary.emplace_back();
            continue;
        }
        const auto& chunkPtr = dictionaryChunks[chunkIndex];
        if (!chunkPtr) {
            dictionary.emplace_back();
            continue;
        }

        Chunk& chunk = *chunkPtr;
        std::shared_lock<std::shared_mutex> lk(chunk.mutex);
        dictionary.push_back(chunk.bucket[localIndex]);
    }
}

void inverted_index::InvertedIndex::rebuildDictionaryFromChunks()
{
    std::lock_guard<std::mutex> g(mapMutex);
    rebuildDictionaryFromChunksLocked();
}

void inverted_index::InvertedIndex::rebuildChunksFromDictionary()
{
    // Вызывать только при удерживаемом mapMutex (load, compact).
    dictionaryChunks.clear();

    for (size_t wid = 0; wid < dictionary.size(); ++wid)
    {
        size_t chunkIndex = wid / CHUNK_SIZE;
        size_t localIndex = wid % CHUNK_SIZE;

        if (chunkIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(chunkIndex + 1);

        if (!dictionaryChunks[chunkIndex])
            dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();

        dictionaryChunks[chunkIndex]->bucket[localIndex] =
            std::move(dictionary[wid]);
    }

    dictionary.clear();
    dictionary.shrink_to_fit();
}




void inverted_index::InvertedIndex::reconstructWordIts()
{
    wordRefs.clear();

    for (size_t chunkIndex = 0; chunkIndex < dictionaryChunks.size(); ++chunkIndex)
    {
        const auto& chunkPtr = dictionaryChunks[chunkIndex];
        if (!chunkPtr) continue;

        Chunk& chunk = *chunkPtr;
        std::shared_lock<std::shared_mutex> lk(chunk.mutex);

        for (size_t localIndex = 0; localIndex < chunk.bucket.size(); ++localIndex)
        {
            const PostingList& posting = chunk.bucket[localIndex];
            if (posting.empty()) continue;

            uint32_t wid = static_cast<uint32_t>(chunkIndex * CHUNK_SIZE + localIndex);

            for (const auto& [docId, cnt] : posting)
                wordRefs[docId].push_back(wid);
        }
    }
}


void inverted_index::InvertedIndex::fixDictionaryHoles()
{

    addToLog("===> Dictionary holes auto-fix started (fixDictionaryHoles)");

    for (FileId fileId = 0; fileId < docPaths.size(); ++fileId)
    {
        const std::wstring& wpath = docPaths.pathById(fileId);


        if (wpath.empty()) continue;

        // Прочитать файл как при индексации (упрощённый вариант)
        std::ifstream file(wpath, std::ios::binary);
        if (!file.is_open()) continue;

        std::vector<char> data;
        file.unsetf(std::ios::skipws);
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        data.reserve(fileSize);

        try {
            data.insert(data.begin(),
                        std::istream_iterator<char>(file),
                        std::istream_iterator<char>());
        } catch (...) { continue; }

        std::stringstream ss;
        std::copy(data.begin(), data.end(), std::ostream_iterator<char>(ss));

        std::set<std::string> realWords;
        auto wordRange = std::ranges::istream_view<std::string>(ss)
                         | std::views::transform([](const std::string& text) {
            auto s = text;
            boost::algorithm::trim_if(s, [](auto c) {
                return OEMCase::iS_not_a_Oem(c) && ispunct(c);
            });
            for (auto& c : s) c = OEMCase::getUpperCharOem(c);
            return s;
        });
        std::ranges::for_each(wordRange, [&realWords](auto&& word) {
            realWords.insert(word);
        });

        std::lock_guard<std::mutex> lock(mapMutex);
        for (const std::string& word : realWords)
        {
            uint32_t wid;
            if (!wordIds.tryGet(word, wid)) continue;

            if (!dictionary[wid].find(fileId)) {
                dictionary[wid][fileId] = 1; // или поставить правильный cnt, если есть возможность
                wordRefs[fileId].push_back(wid);
                addToLog("AUTOFIX: restored word '" + word + "' for file " + std::to_string(fileId));
            }
        }
    }

    addToLog("===> Dictionary holes auto-fix completed (fixDictionaryHoles)");
}


void inverted_index::InvertedIndex::compactMemory()
{
    addToLog("compactMemory: start");

    // Прогоняем основную работу через strand_, чтобы гарантировать
    // отсутствие гонок с processBatch/safeEraseFile (они тоже идут в strand_).
    std::promise<void> done;
    auto fut = done.get_future();

    postTracked(strand_, [this, &done]()
    {
        try
        {
            // 1) PostingList'ы внутри чанков — ужимаем под мьютексом каждого
            //    чанка. Указатели берём один раз под mapMutex, чтобы не
            //    держать его на время длинного прохода.
            std::vector<Chunk*> chunks;
            {
                std::lock_guard<std::mutex> g(mapMutex);
                chunks.reserve(dictionaryChunks.size());
                for (auto& up : dictionaryChunks)
                    chunks.push_back(up.get());
            }

            size_t shrunkPostings = 0;
            for (Chunk* ch : chunks)
            {
                if (!ch) continue;
                std::unique_lock<std::shared_mutex> lk(ch->mutex);
                for (auto& posting : ch->bucket)
                {
                    if (!posting.empty()) {
                        posting.shrink_to_fit();
                        ++shrunkPostings;
                    }
                }
            }

            // 2) Внешние векторы под mapMutex.
            {
                std::lock_guard<std::mutex> g(mapMutex);
                dictionaryChunks.shrink_to_fit();

                if (!dictionary.empty()) {
                    for (auto& pl : dictionary)
                        pl.shrink_to_fit();
                    dictionary.shrink_to_fit();
                }
            }

            // 3) Структуры, изменяемые строго в strand_:
            //    wordRefs, wordIds, docPaths.
            for (auto& [fileId, vec] : wordRefs)
                vec.shrink_to_fit();
            wordRefs.rehash(0);

            wordIds.shrinkToFit();
            docPaths.shrinkToFit();

            addToLog("compactMemory: shrunk " +
                     std::to_string(shrunkPostings) + " postings");

            done.set_value();
        }
        catch (...)
        {
            try { done.set_exception(std::current_exception()); } catch (...) {}
        }
    });

    try { fut.get(); }
    catch (const std::exception& e) {
        addToLog(std::string("compactMemory: EXCEPTION ") + e.what());
        return;
    }
    catch (...) {
        addToLog("compactMemory: unknown exception");
        return;
    }

    // 4) Подсказка ОС: подрезать рабочий набор. Это косметика —
    //    фактическое heap-потребление определяется аллокатором, а не
    //    Working Set'ом. Но для GetProcessMemoryInfo цифра упадёт ближе
    //    к реальному использованию.
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));

    const size_t memBytes = process_memory();
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "compactMemory: done, process_memory="
        << (memBytes / 1024 / 1024) << " MB";
    addToLog(oss.str());
}

void inverted_index::InvertedIndex::saveIndex() const {
    // Защита от параллельных вызовов saveIndex
    std::lock_guard<std::mutex> saveLock(saveMutex);
    
    addToLog("saveIndex() -> start");
    
    // Проверяем, не выполняется ли обновление (но не блокируем его)
    if (work.load(std::memory_order_acquire)) {
        addToLog("saveIndex() -> SKIP: update in progress");
        return;
    }

    try {
        ensureSerializer();
        if (!serializer_) {
            addToLog("saveIndex() -> ERROR: serializer not configured");
            return;
        }

        // Live-зеркало (SQLite): данные уже записаны инкрементально по ходу
        // индексации/удаления. Полный снапшот не нужен — достаточно сбросить
        // WAL на диск контрольной точкой. Это убирает пик ОЗУ от материализации.
        if (serializer_->supportsLiveUpdates()) {
            serializer_->flushPending();
            serializer_->checkpoint();
            addToLog("saveIndex() -> live mirror: flushPending + checkpoint done");
            return;
        }

        // Полный снапшот (Boost): стримим из chunks внутри сериализатора,
        // dictionary материализуем только если он пуст.
        InvertedIndex* nonConstThis = const_cast<InvertedIndex*>(this);
        bool rebuiltFromChunks = false;
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            if (dictionary.empty()) {
                nonConstThis->rebuildDictionaryFromChunksLocked();
                rebuiltFromChunks = true;
            }
        }
        addToLog(std::string("saveIndex() -> trace: rebuild_from_chunks=") +
                 (rebuiltFromChunks ? "yes" : "no"));

        addToLog("saveIndex() -> trace: serialize begin");
        serializer_->save(*this);
        addToLog("saveIndex() -> trace: serialize end");

        addToLog("saveIndex() -> saved successfully");

        // Очищаем только временную flat-копию legacy chunks. Для batch это
        // основное единое in-memory представление, его удалять нельзя.
        if (rebuiltFromChunks) {
            std::lock_guard<std::mutex> lock(mapMutex);
            dictionary.clear();
            dictionary.shrink_to_fit();
        }
    }
    catch (const std::exception& e) {
        addToLog("saveIndex() -> EXCEPTION: " + std::string(e.what()));
    }
    catch (...) {
        addToLog("saveIndex() -> EXCEPTION: unknown error");
    }
}

void inverted_index::InvertedIndex::beginAsyncOperation()
{
    std::lock_guard<std::mutex> lock(asyncMutex_);
    ++activeAsyncOperations_;
}

void inverted_index::InvertedIndex::finishAsyncOperation() noexcept
{
    std::lock_guard<std::mutex> lock(asyncMutex_);
    if (activeAsyncOperations_ > 0)
        --activeAsyncOperations_;
    if (activeAsyncOperations_ == 0)
        asyncCondition_.notify_all();
}

void inverted_index::InvertedIndex::requestStop() noexcept
{
    stopping_.store(true, std::memory_order_release);
    deferredPaths_.clear();
}

void inverted_index::InvertedIndex::waitForIdle()
{
    std::unique_lock<std::mutex> lock(asyncMutex_);
    asyncCondition_.wait(lock, [this]() {
        return activeAsyncOperations_ == 0;
    });
}

inverted_index::InvertedIndex::~InvertedIndex()
{
    requestStop();
    waitForIdle();
}

inverted_index::InvertedIndex::InvertedIndex(boost::asio::thread_pool& cpu_pool,
                                             boost::asio::io_context& io_commit,
                                             int maxParallelReaders,
                                             int fileIndexingTimeoutSec,
                                             IndexStorageConfig storage)
        : io_commit_(io_commit)
        , cpu_pool_(cpu_pool)
        , strand_(boost::asio::make_strand(io_commit_))
        , storage_(std::move(storage))
{
        maxParallelReaders_ = maxParallelReaders;
        if (maxParallelReaders > 0)
            readSlots_.emplace(static_cast<std::ptrdiff_t>(maxParallelReaders));

        fileIndexingTimeoutSec_ = std::max(1, fileIndexingTimeoutSec);

        if (storage_.fullIndexStrategy == FullIndexStrategy::Batch) {
            batchBuilder_ = std::make_unique<batch::BatchIndexBuilder>(
                batch::BatchIndexOptions{
                    storage_.batchReaderThreads,
                    storage_.batchIndexerThreads,
                    storage_.batchQueueMemoryBytes});
        }

        addToLog("InvertedIndex: maxParallelReaders=" +
                 std::to_string(maxParallelReaders) +
                 ", fileIndexingTimeoutSec=" +
                 std::to_string(fileIndexingTimeoutSec_) +
                 ", sqlite_mirror_flush_interval_sec=" +
                 std::to_string(storage_.sqliteMirrorFlushIntervalSec) +
                 ", sqlite_mirror_max_pending_ops=" +
                 std::to_string(storage_.sqliteMirrorMaxPendingOps) +
                 ", sqlite_load_threads=" +
                 std::to_string(storage_.sqliteLoadThreads) +
                 ", sqlite_precount_postings=" +
                 std::to_string(storage_.sqlitePrecountPostings) +
                 ", full_index_strategy=" +
                 std::string(toString(storage_.fullIndexStrategy)) +
                 ", batch_reader_threads=" +
                 std::to_string(storage_.batchReaderThreads) +
                 ", batch_indexer_threads=" +
                 std::to_string(storage_.batchIndexerThreads) +
                 ", batch_queue_memory_bytes=" +
                 std::to_string(storage_.batchQueueMemoryBytes));

        ensureSerializer();
        try {
            if (serializer_ && serializer_->exists()) {
                serializer_->load(*this);
                dictionary.clear();
                dictionary.shrink_to_fit();
            }
        } catch (const std::exception& e) {
            addToLog(std::string("InvertedIndex: load EXCEPTION: ") + e.what());
            throw;
        } catch (...) {
            addToLog("InvertedIndex: load unknown exception");
            throw;
        }

        // Live-writer запускаем только после завершения восстановления.
        // Так загрузчик не конкурирует со вторым SQLite-соединением за схему
        // и получает согласованный снимок базы.
        if (serializer_ && serializer_->supportsLiveUpdates())
        {
            try { serializer_->openLive(); }
            catch (const std::exception& e) {
                addToLog(std::string("InvertedIndex: openLive EXCEPTION: ") + e.what());
                throw;
            }
            catch (...) {
                addToLog("InvertedIndex: openLive unknown exception");
                throw;
            }
        }
}

void inverted_index::InvertedIndex::ensureSerializer() const
{
    if (serializer_)
        return;

    switch (storage_.kind)
    {
    case IndexSerializationKind::BoostBinary:
        serializer_ = std::make_unique<BoostIndexSerializer>(storage_.path);
        break;
    case IndexSerializationKind::SQLite:
        serializer_ = std::make_unique<SQLiteIndexSerializer>(
            storage_.path,
            LiveMirrorConfig{
                storage_.sqliteMirrorFlushIntervalSec,
                storage_.sqliteMirrorMaxPendingOps,
                storage_.sqliteLoadThreads,
                storage_.sqlitePrecountPostings});
        break;
    default:
        serializer_ = std::make_unique<BoostIndexSerializer>(storage_.path);
        break;
    }

}

void inverted_index::InvertedIndex::compact(double thresholdPercent)
{
    // Внешний вызов compact() должен лишь поставить задачу в strand.
    postTracked(strand_, [this, thresholdPercent]()
    {
        // Проверяем, не выполняется ли обновление (без блокировки, чтобы избежать deadlock)
        if (work.load(std::memory_order_acquire)) {
            addToLog("COMPACT: SKIP - update in progress, will retry later");
            return;
        }

        addToLog("COMPACT: started in strand");

        // Пересобираем dictionary из chunks перед compact (если он пустой)
        // Используем try_lock для updateMutex, чтобы не блокировать обновления надолго
        std::unique_lock<std::mutex> updateLock(updateMutex, std::try_to_lock);
        if (!updateLock.owns_lock()) {
            addToLog("COMPACT: SKIP - update lock busy, will retry later");
            return;
        }

        // Двойная проверка после блокировки
        if (work.load(std::memory_order_acquire)) {
            addToLog("COMPACT: SKIP - update started during lock acquisition");
            return;
        }

        size_t holes = 0;
        size_t dict_size = 0;
        size_t real_words = 0;
        bool chunksAreActive = false;
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            real_words = wordIds.size();
            dict_size = real_words;
            chunksAreActive = dictionary.empty();

            if (!chunksAreActive)
            {
                for (size_t wid = 0; wid < dict_size; ++wid)
                {
                    if (wid >= dictionary.size() || dictionary[wid].empty())
                        ++holes;
                }
            }
            else
            {
                for (size_t chunkIndex = 0;
                     chunkIndex < dictionaryChunks.size();
                     ++chunkIndex)
                {
                    const auto& chunk = dictionaryChunks[chunkIndex];
                    const size_t wordBegin = chunkIndex * CHUNK_SIZE;
                    if (wordBegin >= dict_size)
                        break;
                    const size_t wordsInChunk = std::min(
                        CHUNK_SIZE,
                        dict_size - wordBegin);

                    if (!chunk)
                    {
                        holes += wordsInChunk;
                        continue;
                    }

                    std::shared_lock<std::shared_mutex> chunkLock(
                        chunk->mutex);
                    for (size_t localIndex = 0;
                         localIndex < wordsInChunk;
                         ++localIndex)
                    {
                        if (chunk->bucket[localIndex].empty())
                            ++holes;
                    }
                }

                const size_t representedWords = std::min(
                    dict_size,
                    dictionaryChunks.size() * CHUNK_SIZE);
                holes += dict_size - representedWords;
            }
        }

        double hole_percent = (dict_size > 0) ? (holes * 100.0 / dict_size) : 0.0;

        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << "COMPACT: holes=" << holes
            << ", dict_size=" << dict_size
            << ", real_words=" << real_words
            << ", hole_percent=" << std::fixed << std::setprecision(2)
            << hole_percent << "%"
            << ", threshold=" << std::fixed << std::setprecision(2)
            << thresholdPercent << "%";

        // Нужно ли проводить compact? Используем порог из настроек
        if (holes == 0 || hole_percent < thresholdPercent)
        {
            oss << " -> SKIP (below threshold)";
            addToLog(oss.str());
            return;
        }

        oss << " -> RUN";
        addToLog(oss.str());

        if (chunksAreActive)
            rebuildDictionaryFromChunks();

        // Блокируем доступ к dictionary во время compact
        std::lock_guard<std::mutex> lock(mapMutex);

        // === 1. Подготовить новые структуры ===
        std::vector<PostingList> newDict;
        newDict.reserve(dictionary.size() - holes);

        std::unordered_map<uint32_t, uint32_t> remap; // oldWid → newWid

        // wordIds переходим на новые структуры
        std::unordered_map<std::string, uint32_t> new_word2id;
        std::vector<std::string> new_id2word;

        size_t wordIdsSize = wordIds.size();
        for (uint32_t oldWid = 0; oldWid < dictionary.size(); ++oldWid)
        {
            auto& post = dictionary[oldWid];
            if (post.empty())
                continue; // пропускаем дырку

            // Проверяем, что oldWid существует в wordIds
            if (oldWid >= wordIdsSize) {
                addToLog("COMPACT: WARNING - oldWid " + std::to_string(oldWid) + 
                        " >= wordIds.size() (" + std::to_string(wordIdsSize) + "), skipping");
                continue;
            }

            auto newWid = static_cast<uint32_t>(newDict.size());
            remap[oldWid] = newWid;
            newDict.push_back(std::move(post));

            // Безопасное получение слова по ID
            const std::string& word = wordIds.byId(oldWid);
            new_word2id[word] = newWid;
            new_id2word.push_back(word);
        }

        // Переставляем словарь
        dictionary.swap(newDict);

        // === 2. Пересборка wordIds ===
        wordIds.rebuild(std::move(new_word2id), std::move(new_id2word));

        // === 3. Пересборка wordRefs с обработкой отсутствующих ID ===
        size_t remapErrors = 0;
        for (auto& [fileId, vec] : wordRefs)
        {
            for (uint32_t& wid : vec)
            {
                auto it = remap.find(wid);
                if (it != remap.end()) {
                    wid = it->second;
                } else {
                    // Если wid не найден в remap, это ошибка данных
                    remapErrors++;
                    addToLog("COMPACT: ERROR - wid " + std::to_string(wid) + 
                            " not found in remap for fileId " + std::to_string(fileId));
                    // Устанавливаем недопустимое значение для последующей очистки
                    wid = UINT32_MAX;
                }
            }
        }

        // Очищаем недопустимые значения из wordRefs
        if (remapErrors > 0) {
            const uint32_t INVALID_WID = UINT32_MAX;
            for (auto& [fileId, vec] : wordRefs) {
                vec.erase(
                    std::remove(vec.begin(), vec.end(), INVALID_WID),
                    vec.end()
                );
            }
            addToLog("COMPACT: removed " + std::to_string(remapErrors) + 
                    " invalid word references");
        }

        // === 4. Пересборка chunks из dictionary ===
        rebuildChunksFromDictionary();

        addToLog("COMPACT: done, active_representation=chunks, words=" +
                 std::to_string(wordIds.size()));
    });
}

void inverted_index::InvertedIndex::safeEraseFile(FileId fileId)
{
    /*  для каждой Wid-очереди стираем запись hash, если она есть
        (никаких erase по несуществующему индексу)  */

    postTracked(strand_,[this,fileId]()
    {
        safeEraseFileInternal(fileId);
    });

}

bool inverted_index::InvertedIndex::enqueueFileUpdate(const std::wstring& path)
{
    namespace fs = std::filesystem;

    if (stopping_.load(std::memory_order_acquire))
        return false;

    std::unique_lock<std::mutex> updateLock(
        updateMutex, std::try_to_lock);
    if (!updateLock.owns_lock() ||
        work.load(std::memory_order_acquire))
    {
        return deferPointPath(path, "enqueueFileUpdate");
    }

    prepareLegacyMutableState();

    std::wcout << L"[enqueueFileUpdate] Request for path: " << path << std::endl;

    if (!fs::exists(path) || !fs::is_regular_file(path))
    {
        std::wcout << L"[enqueueFileUpdate] Skip — not a regular file: " << path << std::endl;
        return false;
    }

    // Метаданные читаем в любом потоке
    fs::file_time_type ts = fs::last_write_time(path);
    uint64_t sz = fs::file_size(path);

    //
    // Работа с docPaths и удаление — строго в strand
    //
    postTracked(strand_,
                      [this, path, ts, sz]()
                      {
                          std::wcout << L"[strand/docPaths] Upserting: " << path << std::endl;

                          auto [fileId, changed] = docPaths.upsert(path, ts, sz);

                          if (!changed)
                          {
                              std::wcout << L"[strand/docPaths] File unchanged — stopping update.\n";
                              return;
                          }

                          bool isNewFile = (wordRefs.find(fileId) == wordRefs.end());

                          if (!isNewFile)
                          {
                              std::wcout << L"[strand/index] Old file changed, erasing old postings...\n";
                              safeEraseFileInternal(fileId);
                          }
                          else
                          {
                              std::wcout << L"[strand/index] New file detected, skipping erase.\n";
                          }

                          //
                          // ---- Создаём промис и future ДЛЯ ЭТОГО КОНКРЕТНОГО ФАЙЛА ----
                          //
                          auto promise = std::make_shared<std::promise<void>>();
                          auto future  = promise->get_future();

                          // Здесь по желанию можно future сохранить в твоём менеджере
                          // или вернуть наружу, если enqueueFileUpdate будет возвращать future.
                          // Пока оставляем так, future будет уничтожен → нормально.

                          //
                          // ---- Запускаем многопоточную индексацию файла ----
                          //
                          std::wcout << L"[strand/index] Scheduling fileIndexing job for id=" << fileId << std::endl;

                          postTracked(cpu_pool_, [this, fileId, promise]()
                          {
                              try
                              {
                                  std::wcout << L"[fileIndexing] Started for id=" << fileId << std::endl;

                                  // 🔥 ВАЖНО: fileIndexing теперь принимает promise
                                  fileIndexing(fileId, promise);

                                  std::wcout << L"[fileIndexing] Dispatched batch for id=" << fileId << std::endl;
                              }
                              catch (...)
                              {
                                  std::wcerr << L"[fileIndexing EXC] id=" << fileId << std::endl;
                                  try { promise->set_exception(std::current_exception()); } catch(...) {}
                              }
                          });

                          //
                          // Теперь fileIndexing → создаёт batch → batch.promise = promise
                          // → processBatch завершит promise->set_value() или set_exception()
                          //
                      });

    return true;
}


bool inverted_index::InvertedIndex::enqueueFileDeletion(const std::wstring& path)
{
    if (stopping_.load(std::memory_order_acquire))
        return false;

    std::unique_lock<std::mutex> updateLock(
        updateMutex, std::try_to_lock);
    if (!updateLock.owns_lock() ||
        work.load(std::memory_order_acquire))
    {
        return deferPointPath(path, "enqueueFileDeletion");
    }

    std::wcout << L"[enqueueFileDeletion] Request for path: " << path << std::endl;

    FileId id;

    {
        std::lock_guard<std::mutex> lock(mapMutex); // только для tryGetId, если оно требует
        if (!docPaths.tryGetId(path, id))
        {
            std::wcout << L"[enqueueFileDeletion] Skip — unknown file: " << path << std::endl;
            return false;
        }
    }

    // Вечный след: помечаем файл как удалённый, но НЕ стираем постинги
    // (ни в памяти, ни в SQLite). Поиск продолжит находить файл с флагом
    // "отсутствует". wordRefs сохраняем — пригодится при реактивации/замене.
    postTracked(strand_, [this, id, path]() {

        std::wcout << L"[strand/docPaths] markRemoved (soft, eternal trace): "
                   << path << L" (id=" << id << L")" << std::endl;
        docPaths.markRemoved(id);

        ensureSerializer();
        if (serializer_ && serializer_->supportsLiveUpdates())
        {
            try { serializer_->markFileDeleted(id); }
            catch (const std::exception& e) {
                addToLog(std::string("enqueueFileDeletion: markFileDeleted EXCEPTION: ") + e.what());
            }
            catch (...) {
                addToLog("enqueueFileDeletion: markFileDeleted unknown exception");
            }
        }
    });

    return true;
}

bool inverted_index::InvertedIndex::deferPointPath(
        const std::wstring& path, const char* source)
{
    if (stopping_.load(std::memory_order_acquire))
        return false;

    const bool inserted = deferredPaths_.defer(path);
    if (stopping_.load(std::memory_order_acquire)) {
        deferredPaths_.clear();
        return false;
    }

    addToLog(
        std::string(source) + " queued deferred path=" +
        encoding::wstring_to_utf8(path) +
        (inserted ? " action=inserted" : " action=coalesced"));
    return true;
}

void inverted_index::InvertedIndex::drainDeferredPointPaths()
{
    if (stopping_.load(std::memory_order_acquire)) {
        deferredPaths_.clear();
        return;
    }

    namespace fs = std::filesystem;
    const std::size_t replayed = deferredPaths_.drainOnce(
        [this](const std::wstring& path) {
        if (stopping_.load(std::memory_order_acquire))
            return;

        std::error_code error;
        const bool regular = fs::is_regular_file(path, error);
        if (!error && regular)
            enqueueFileUpdate(path);
        else
            enqueueFileDeletion(path);
    });

    if (replayed != 0)
        addToLog("deferred point replay complete paths=" +
                 std::to_string(replayed));
}
