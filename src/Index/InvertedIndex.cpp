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

    // создаём/расширяем чанк под глобальным мьютексом, как в getPostingList
    {
        std::lock_guard<std::mutex> g(mapMutex);
        if (chunkIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(chunkIndex + 1);
        if (!dictionaryChunks[chunkIndex])
            dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();
    }

    Chunk& chunk = *dictionaryChunks[chunkIndex];

    std::unique_lock<std::shared_mutex> lk(chunk.mutex);
    chunk.bucket[localIndex][t.fileId] += t.count;
}

void inverted_index::InvertedIndex::commitChunkMap(
        const std::unordered_map<size_t, std::vector<PostingTask>>& chunkMap)
{
    // === 1. (Редко) расширяем массив чанков ОДИН раз ===
    {
        std::lock_guard<std::mutex> g(mapMutex);

        size_t maxIndex = 0;
        for (auto& [cid, _] : chunkMap)
            if (cid > maxIndex) maxIndex = cid;

        if (maxIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(maxIndex + 1);

        for (auto& [cid, _] : chunkMap)
            if (!dictionaryChunks[cid])
                dictionaryChunks[cid] = std::make_unique<Chunk>();
    }

    // === 2. Обновляем чанки с минимальными lock ===
    for (auto& [cid, tasks] : chunkMap)
    {
        Chunk& ch = *dictionaryChunks[cid];
        std::unique_lock<std::shared_mutex> lk(ch.mutex);

        for (auto& t : tasks)
        {
            size_t local = t.wordId % CHUNK_SIZE;
            ch.bucket[local][t.fileId] += t.count;
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

        // 1.1. Преобразуем слова → wid + суммируем counts
        for (auto& [word, count] : batch.list)
        {
            uint32_t wid = wordIds.getId(word);
            widMap[wid] += count;

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
        boost::asio::post(cpu_pool_, [this,
                chunkMap = std::move(chunkMap),
                promise = batch.promise]()
        {
            try
            {
                commitChunkMap(chunkMap);
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
    uint32_t wid;
    if (!wordIds.tryGet(w, wid))
        return std::nullopt;

    const size_t chunkIndex = wid / CHUNK_SIZE;
    const size_t localIndex = wid % CHUNK_SIZE;

    if (chunkIndex >= dictionaryChunks.size())
        return std::nullopt;

    const auto& chunkPtr = dictionaryChunks[chunkIndex];
    if (!chunkPtr)
        return std::nullopt;

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
    boost::asio::post(strand_, [this, batch = std::move(batch)]() mutable
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
    boost::asio::post(strand_, [this, list]() {
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
        for (uint32_t wid : widList)
        {
            size_t chunkIndex = wid / CHUNK_SIZE;
            size_t localIndex = wid % CHUNK_SIZE;

            if (chunkIndex >= dictionaryChunks.size())
                continue;

            auto& chunkPtr = dictionaryChunks[chunkIndex];
            if (!chunkPtr)
                continue;

            Chunk& chunk = *chunkPtr;
            std::unique_lock<std::shared_mutex> lk(chunk.mutex);
            auto& posting = chunk.bucket[localIndex];
            if (!posting.empty())
                posting.erase(fileId);
        }

        wordRefs.erase(itRefs);
    }
    else
    {
        // fallback: полный обход (редкий случай)
        for (auto& chunkPtr : dictionaryChunks)
        {
            if (!chunkPtr) continue;
            Chunk& chunk = *chunkPtr;

            std::unique_lock<std::shared_mutex> lk(chunk.mutex);
            for (auto& posting : chunk.bucket)
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
    // Защита от параллельных вызовов
    std::lock_guard<std::mutex> updateLock(updateMutex);
    
    addToLog("updateDocumentBase() → start");
    
    // Проверяем, не выполняется ли уже обновление
    bool expected = false;
    if (!work.compare_exchange_strong(expected, true)) {
        addToLog("updateDocumentBase() → SKIP: update already in progress");
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
    UpdatePack pack = docPaths.getUpdate({ vecPaths.begin(), vecPaths.end() });

    std::vector<FileId> toDelete = pack.removed;
    toDelete.insert(toDelete.end(), pack.updated.begin(), pack.updated.end());

    std::vector<FileId> toIndex = pack.added;
    toIndex.insert(toIndex.end(), pack.updated.begin(), pack.updated.end());

    std::ostringstream diffLog;
    diffLog << "diff: +"  << pack.added.size()
            << ", upd "   << pack.updated.size()
            << ", del "   << pack.removed.size();
    addToLog(diffLog.str());

    // Нет работы — сразу готовый future
    if (toDelete.empty() && toIndex.empty())
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

            boost::asio::post(strand_, [this, &toDelete, &delPromise]()
            {
                try {
                    for (FileId fileId : toDelete)
                        safeEraseFileInternal(fileId);

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

            boost::asio::post(cpu_pool_,
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
        for (auto& ff : fileFutures)
        {
            using namespace std::chrono_literals;

            if (ff.fut.wait_for(60s) != std::future_status::ready)
            {
                logIndexError(ff.path, "TIMEOUT waiting file future (60s)");
                continue;
            }

            try { ff.fut.get(); }
            catch (const std::exception& e) { logIndexError(ff.path, e.what()); }
            catch (...) { logIndexError(ff.path, "unknown exception"); }
        }

        addToLog("updateDocumentBase: all fileIndexing done");

        // ------------------------------------------------------------------
        // 5. ФИНАЛ — saveIndex (тут же, линейно)
        // ------------------------------------------------------------------
        auto startTime = std::chrono::steady_clock::now();
        addToLog("FINAL: before saveIndex");
        saveIndex();
        auto saveTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        addToLog("FINAL: after saveIndex, took " + std::to_string(saveTime) + " ms");

        // Log dictionary statistics and memory usage
        auto stats = getStats();
        size_t memBytes = process_memory();
        size_t dict_size = dictionaryChunks.size() * CHUNK_SIZE;  // Total dictionary slots
        double hole_percent = (dict_size > 0) ? (stats.emptyPostings * 100.0 / dict_size) : 0.0;
        
        std::ostringstream statsLog;
        statsLog << "DICTIONARY STATS: unique_words=" << stats.uniqueWords
                 << ", total_postings=" << stats.totalPostings
                 << ", total_files=" << stats.totalFiles
                 << ", dictionary_slots=" << dict_size
                 << ", holes=" << stats.emptyPostings
                 << ", hole_percent=" << std::fixed << std::setprecision(2) << hole_percent << "%"
                 << ", dictionary_memory=" << (stats.memoryBytes / 1024 / 1024) << " MB"
                 << ", process_memory=" << (memBytes / 1024 / 1024) << " MB";
        addToLog(statsLog.str());

        finalPromise->set_value();
        // work будет сброшен автоматически через WorkGuard
    }
    catch (...)
    {
        try { finalPromise->set_exception(std::current_exception()); } catch (...) {}
        // work будет сброшен автоматически через WorkGuard
    }

    return future;
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

        std::ifstream file(fullPath.c_str(), std::ios::binary);
        if (!file.is_open())
        {
            addToLog("fileIndexing: cannot open file " +
                     encoding::wstring_to_utf8(fullPath));

            promise->set_exception(
                    std::make_exception_ptr(
                            std::runtime_error("file not found")));
            return;
        }

        // --- словарь частот ---
        robin_hood::unordered_map<std::string, size_t> freqWordFile;

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
                        // -------- 🔥 ТВОЯ OEM-ЛОГИКА --------

                        std::string w;
                        w.assign(tok.data(), tok.size());  // быстрее, чем std::string(tok)

                        OEMFastTokenizer::normalizeToken(w);

                        if (!w.empty())
                            ++freqWordFile[w];
                    });
        }

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
    
    // Реальное количество уникальных слов
    stats.uniqueWords = wordIds.size();
    stats.totalFiles = docPaths.size();
    
    // Подсчет постингов из chunks (без длительных блокировок)
    size_t totalPostings = 0;
    size_t emptyPostings = 0;
    size_t memoryBytes = 0;
    
    // Проходим по chunks с минимальными блокировками
    for (const auto& chunkPtr : dictionaryChunks)
    {
        if (!chunkPtr) {
            // Пустой chunk = CHUNK_SIZE пустых постингов
            emptyPostings += CHUNK_SIZE;
            continue;
        }
        
        const Chunk& chunk = *chunkPtr;
        // Используем try_lock для неблокирующего чтения где возможно
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

void inverted_index::InvertedIndex::rebuildDictionaryFromChunks()
{
    std::lock_guard<std::mutex> g(mapMutex);
    dictionary.clear();
    dictionary.reserve(dictionaryChunks.size() * CHUNK_SIZE);

    for (const auto & chunkPtr : dictionaryChunks)
    {
        if (!chunkPtr) {
            // Добавляем пустые posting-листы
            for (size_t i=0; i<CHUNK_SIZE; ++i)
                dictionary.emplace_back();
            continue;
        }

        Chunk& chunk = *chunkPtr;
        std::shared_lock<std::shared_mutex> lk(chunk.mutex);


        for (size_t localIndex = 0; localIndex < CHUNK_SIZE; ++localIndex)
            dictionary.push_back(chunk.bucket[localIndex]);
    }
}

void inverted_index::InvertedIndex::rebuildChunksFromDictionary()
{
    dictionaryChunks.clear();

    for (size_t wid = 0; wid < dictionary.size(); ++wid)
    {
        size_t chunkIndex = wid / CHUNK_SIZE;
        size_t localIndex = wid % CHUNK_SIZE;

        if (chunkIndex >= dictionaryChunks.size())
            dictionaryChunks.resize(chunkIndex + 1);

        if (!dictionaryChunks[chunkIndex])
            dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();

        dictionaryChunks[chunkIndex]->bucket[localIndex] = dictionary[wid];
    }
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


void inverted_index::InvertedIndex::saveIndex() const {
    // Защита от параллельных вызовов saveIndex
    std::lock_guard<std::mutex> saveLock(saveMutex);
    
    addToLog("saveIndex() → start");
    
    // Проверяем, не выполняется ли обновление (но не блокируем его)
    if (work.load(std::memory_order_acquire)) {
        addToLog("saveIndex() → SKIP: update in progress");
        return;
    }

    try {
        // Пересобираем dictionary из chunks перед сохранением (если он пустой)
        // Используем const_cast для вызова не-const метода
        InvertedIndex* nonConstThis = const_cast<InvertedIndex*>(this);
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            if (dictionary.empty()) {
                nonConstThis->rebuildDictionaryFromChunks();
            }
        }

        // Сохраняем индекс
        std::ofstream ofs("inverted_index3.dat", std::ios::binary);
        if (ofs.is_open()) {
            boost::archive::binary_oarchive oa(ofs);
            {
                std::lock_guard<std::mutex> lock(mapMutex);
                oa << *this;
            }
            ofs.close();
            addToLog("saveIndex() → saved successfully");
        } else {
            addToLog("saveIndex() → ERROR: Failed to open file for saving index.");
        }

        // Очищаем dictionary после сохранения
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            dictionary.clear();
            dictionary.shrink_to_fit();
        }
    }
    catch (const std::exception& e) {
        addToLog("saveIndex() → EXCEPTION: " + std::string(e.what()));
    }
    catch (...) {
        addToLog("saveIndex() → EXCEPTION: unknown error");
    }
}

inverted_index::InvertedIndex::~InvertedIndex() {
    saveIndex();
}

inverted_index::InvertedIndex::InvertedIndex(boost::asio::thread_pool& cpu_pool, boost::asio::io_context& io_commit)
        : io_commit_(io_commit)
        , strand_(boost::asio::make_strand(io_commit_))
        , cpu_pool_(cpu_pool)
{
        // Проверка наличия файла с индексом
        if (std::filesystem::exists("inverted_index3.dat")) {
            // Если файл существует, загружаем данные
            std::ifstream ifs("inverted_index3.dat", std::ios::binary);
            if (ifs.is_open()) {
                boost::archive::binary_iarchive ia(ifs);
                ia >> *this; // Загрузка данных через Boost.Serialization
            }

            dictionary.clear();
            dictionary.shrink_to_fit();

        }
}

void inverted_index::InvertedIndex::compact(double thresholdPercent)
{
    // Внешний вызов compact() должен лишь поставить задачу в strand.
    boost::asio::post(strand_, [this, thresholdPercent]()
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

        if (dictionary.empty()) {
            rebuildDictionaryFromChunks();
        }

        size_t holes = 0;
        for (const auto& post : dictionary)
            if (post.empty())
                ++holes;

        // Используем wordIds.size() для реального размера словаря
        size_t dict_size = dictionary.size();
        size_t real_words = wordIds.size();
        double hole_percent = (dict_size > 0) ? (holes * 100.0 / dict_size) : 0.0;

        std::ostringstream oss;
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
            oss << " → SKIP (below threshold)";
            addToLog(oss.str());
            return;
        }

        oss << " → RUN";
        addToLog(oss.str());

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

        addToLog("COMPACT: done, new dict size=" + std::to_string(dictionary.size()) +
                ", new wordIds size=" + std::to_string(wordIds.size()));
    });
}

void inverted_index::InvertedIndex::safeEraseFile(FileId fileId)
{
    /*  для каждой Wid-очереди стираем запись hash, если она есть
        (никаких erase по несуществующему индексу)  */

    boost::asio::post(strand_,[this,fileId]()
    {
        safeEraseFileInternal(fileId);
    });

}

bool inverted_index::InvertedIndex::enqueueFileUpdate(const std::wstring& path)
{
    namespace fs = std::filesystem;

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
    boost::asio::post(strand_,
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

                          boost::asio::post(cpu_pool_, [this, fileId, promise]()
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

    // помечаем файл как удалённый
    boost::asio::post(strand_, [this, id, path]() {

        std::wcout << L"[strand/docPaths] markRemoved: " << path << L" (id=" << id << L")" << std::endl;
        docPaths.markRemoved(id);

    });

    // чистим постинги и wordRefs
    boost::asio::post(strand_, [this, id]() {

        std::wcout << L"[strand/dictionary] Starting cleanup for id=" << id << std::endl;

        auto refIt = wordRefs.find(id);
        if (refIt == wordRefs.end()) {
            std::wcout << L"[strand/dictionary] Already removed, id=" << id << std::endl;
            return;
        }

        const auto &widList = refIt->second;

        for (uint32_t wid : widList)
        {
            const size_t chunkIndex = wid / CHUNK_SIZE;
            const size_t localIndex = wid % CHUNK_SIZE;

            if (chunkIndex >= dictionaryChunks.size())
                continue;

            auto& chunkPtr = dictionaryChunks[chunkIndex];
            if (!chunkPtr)
                continue;

            Chunk& chunk = *chunkPtr;
            std::unique_lock<std::shared_mutex> lk(chunk.mutex);

            auto& posting = chunk.bucket[localIndex];
            posting.erase(id);
            // если пустой — просто остаётся пустым; чистить ещё где-то не надо
        }

        std::wcout << L"[strand/dictionary] Cleaned "
                   << widList.size() << L" postings for id=" << id << std::endl;

        auto it = wordRefs.find(id);
        if (it != wordRefs.end()) {
            wordRefs.erase(it);
            std::wcout << L"[strand/wordRefs] Erased id=" << id << std::endl;
        } else {
            std::wcout << L"[strand/wordRefs] Already erased id=" << id << std::endl;
        }
    });

    return true;
}
