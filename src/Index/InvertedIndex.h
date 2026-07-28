//
// Created by user on 01.02.2023.
//
#pragma once
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <boost/serialization/access.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/string.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include "WordID.h"
#include "PostingList.h"
#include "robin_hood.h"
#include "MyUtils/OEMCase.h"

#include "DocPaths.h"
#include "IIndexSerializer.h"

namespace search_server
{
    class SearchServer;
    struct RelativeIndex;
}

namespace inverted_index {

    class BoostIndexSerializer;
    class SQLiteIndexSerializer;


    using namespace std;

    using FileId = uint32_t;
    typedef unordered_map<size_t, size_t> mapEntry;
    typedef map<size_t, vector<unordered_map<string,mapEntry>::iterator>> mapDictionaryIterators;


    template<typename Time = chrono::seconds, typename Clock = chrono::high_resolution_clock>
    struct perf_timer {
        /** @param perf_timer Стркуктура повзволяющая получить время выполнения любой функции.
         * Взято из книги "Решение задач на современном C++" Мариуса Бансила.
         * */
        template<typename F, typename... Args>
        static Time duration(F &&f, Args... args) {
            auto start = Clock::now();
            invoke(forward<F>(f), forward<Args>(args)...);
            auto end = Clock::now();
            return chrono::duration_cast<Time>(end - start);
        }
    };



    struct DictionaryStats {
        size_t uniqueWords;      // wordIds.size() - реальное количество уникальных слов
        size_t totalPostings;    // сумма size() всех непустых PostingList
        size_t emptyPostings;    // holes - пустые постинг-листы
        size_t totalFiles;        // docPaths.size()
        size_t memoryBytes;      // примерный размер постингов в памяти
    };

    enum class IndexSerializationKind {
        BoostBinary,
        SQLite
    };

    struct IndexStorageConfig {
        IndexSerializationKind kind = IndexSerializationKind::BoostBinary;
        std::string path = "inverted_index3.dat";
        double sqliteMirrorFlushIntervalSec = 2.0;
        int sqliteMirrorMaxPendingOps = 500;
        int sqliteLoadThreads = 4;
        bool sqlitePrecountPostings = false;
    };

    class InvertedIndex {

        struct PostingBatch {
            FileId fileId;
            std::vector<std::pair<std::string, uint32_t>> list;
            std::shared_ptr<std::promise<void>> promise;
        };

        static void pingIo(boost::asio::io_context& ctx, const char* name);


        std::optional<PostingList> getPostingCopyByWord(const std::string& w) const;


        struct Chunk {
            std::vector<PostingList> bucket;
            mutable std::shared_mutex mutex;  // mutable для блокировки в const методах

            Chunk() : bucket(CHUNK_SIZE) {}
        };

        struct PostingTask {
            FileId   fileId;
            uint32_t wordId;
            uint32_t count;
        };

        struct FileFuture
        {
            FileId id;
            std::wstring path;
            std::future<void> fut;
        };


        static constexpr size_t CHUNK_SIZE = 4096;


        PostingList& getPostingList(uint32_t wid)
        {
            size_t chunkIndex = wid / CHUNK_SIZE;
            size_t localIndex = wid % CHUNK_SIZE;

            std::lock_guard<std::mutex> g(mapMutex);
            if (chunkIndex >= dictionaryChunks.size())
                dictionaryChunks.resize(chunkIndex + 1);

            if (!dictionaryChunks[chunkIndex])
                dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();

            return dictionaryChunks[chunkIndex]->bucket[localIndex];
        }

        [[maybe_unused]] std::shared_mutex& mutexForWord(uint32_t wid)
        {
            size_t chunkIndex = wid / CHUNK_SIZE;

            std::lock_guard<std::mutex> g(mapMutex);
            if (chunkIndex >= dictionaryChunks.size())
                dictionaryChunks.resize(chunkIndex + 1);

            if (!dictionaryChunks[chunkIndex])
                dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();

            return dictionaryChunks[chunkIndex]->mutex;
        }

        void commitSingleWord(const PostingTask& t);


        std::vector<std::unique_ptr<Chunk>> dictionaryChunks;


        atomic<bool> work{};
        DocPaths docPaths;

        mutable mutex mapMutex;
        mutable mutex resizeDicMutex;
        mutable mutex logMutex;
        mutex updateMutex;  // Защита от параллельных вызовов updateDocumentBase
        mutable mutex saveMutex;  // Защита от параллельных вызовов saveIndex


        boost::asio::io_context& io_commit_;
        boost::asio::thread_pool& cpu_pool_;
        boost::asio::strand<boost::asio::io_context::executor_type> strand_;

        /// Ограничитель одновременных читателей файлов; пуст при maxParallelReaders <= 0.
        std::optional<std::counting_semaphore<>> readSlots_;
        /// Конфигурация для логов/диагностики.
        int maxParallelReaders_ = 0;
        /// Таймаут ожидания одной задачи fileIndexing в updateDocumentBase, секунды.
        int fileIndexingTimeoutSec_ = 60;


        using mapEntry = std::unordered_map<size_t, size_t>;   // fileId → count
        WordIdManager wordIds;               // то, что добавили на шаге 1

        std::unordered_map<size_t, std::vector<uint32_t>> wordRefs;

        mutable std::vector<PostingList> dictionary;

        IndexStorageConfig storage_{};
        mutable std::unique_ptr<IIndexSerializer> serializer_;

        void ensureSerializer() const;

        // Доступ сериализатора к внутреннему состоянию (для SQLite/Boost и т.п.)
        friend class BoostIndexSerializer;
        friend class SQLiteIndexSerializer;


        void safeEraseFileInternal(FileId fileId);
        friend class search_server::SearchServer;
        friend class search_server::RelativeIndex;


        void fileIndexing(FileId fileId, std::shared_ptr<std::promise<void>> promise);
        void safeEraseFile(FileId hash);


        void delFromDictionary(const std::vector<FileId>& list);

        void commitChunkMap(
                std::unordered_map<size_t, std::vector<PostingTask>> chunkMap);

        void rebuildDictionaryFromChunks();
        /// Вызывать только при уже удерживаемом mapMutex (см. saveIndex).
        void rebuildDictionaryFromChunksLocked();
        void rebuildChunksFromDictionary();

        static void addToLog(const string &_s) ;
        void reconstructWordIts();
        void compact(double thresholdPercent = 5.0);  // Порог для compact в процентах
        void fixDictionaryHoles();

        /// Ужать все внутренние контейнеры (capacity → size) и попытаться
        /// вернуть страницы рабочему набору ОС. Вызывать по окончании
        /// массовой индексации, когда новых вставок не ожидается.
        void compactMemory();

        // InvertedIndex.h (внутри class InvertedIndex, в private-секции)

        void applyBatchInStrand(PostingBatch batch);
        void processBatch(const PostingBatch& batch);

        friend class boost::serialization::access;

        /// mapMutex снаружи (saveIndex). Вложенный lock здесь даёт deadlock с saveIndex.
        template<class Archive>
        void save(Archive & ar, const unsigned int version)  const {
            ar & dictionary;
            ar & wordIds;
            ar & docPaths;
        }

        std::condition_variable update_end;

        template<class Archive>
        void load(Archive & ar, const unsigned int version) {
            std::lock_guard<std::mutex> lock(mapMutex);
            ar & dictionary;
            ar & wordIds;
            ar & docPaths;
            rebuildChunksFromDictionary();   // ← добавляем
            reconstructWordIts();            // работает по dictionaryChunks
        }

        BOOST_SERIALIZATION_SPLIT_MEMBER()

    public:

        std::future<void> updateDocumentBase(const std::vector<wstring> &vecPaths);
        PostingList getWordCount(const string& word);
        void dictonaryToLog() const;
        DictionaryStats getStats() const;

        /// Файл с данным id помечен удалённым (исчез с диска), но его след
        /// сохранён в индексе и продолжает находиться поиском.
        [[nodiscard]] bool isFileDeleted(uint32_t fileId) const { return docPaths.isDeleted(fileId); }

        /// Путь файла по id (валиден и для удалённых файлов — вечный след).
        [[nodiscard]] const std::wstring& filePathById(uint32_t fileId) const { return docPaths.pathById(fileId); }
        explicit InvertedIndex(boost::asio::thread_pool& cpu_pool,
                               boost::asio::io_context& io_commit,
                               int maxParallelReaders = 0,
                               int fileIndexingTimeoutSec = 60,
                               IndexStorageConfig storage = {});
        bool enqueueFileUpdate(const std::wstring& path);
        bool enqueueFileDeletion(const std::wstring& path);
        ~InvertedIndex();
        void saveIndex() const;
    };

}

