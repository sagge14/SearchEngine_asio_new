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
#include "WordID.h"
#include "PostingList.h"
#include "robin_hood.h"
#include "OEMtoUpper.h"

#include "DocPaths.h"

namespace search_server
{
    class SearchServer;
    struct RelativeIndex;
}

namespace inverted_index {


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
            std::shared_mutex mutex;

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

            if (chunkIndex >= dictionaryChunks.size())
                dictionaryChunks.resize(chunkIndex + 1);

            if (!dictionaryChunks[chunkIndex])
                dictionaryChunks[chunkIndex] = std::make_unique<Chunk>();

            return dictionaryChunks[chunkIndex]->bucket[localIndex];
        }

        [[maybe_unused]] std::shared_mutex& mutexForWord(uint32_t wid)
        {
            size_t chunkIndex = wid / CHUNK_SIZE;
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


        boost::asio::io_context& io_commit_;
        boost::asio::thread_pool& cpu_pool_;
        boost::asio::strand<boost::asio::io_context::executor_type> strand_;


        using mapEntry = std::unordered_map<size_t, size_t>;   // fileId → count
        WordIdManager wordIds;               // то, что добавили на шаге 1

        std::unordered_map<size_t, std::vector<uint32_t>> wordRefs;

        mutable std::vector<PostingList> dictionary;


        void safeEraseFileInternal(FileId fileId);
        friend class search_server::SearchServer;
        friend class search_server::RelativeIndex;


        void fileIndexing(FileId fileId, std::shared_ptr<std::promise<void>> promise);
        void safeEraseFile(FileId hash);


        void delFromDictionary(const std::vector<FileId>& list);

        void commitChunkMap(
                const std::unordered_map<size_t, std::vector<PostingTask>>& chunkMap);

        void rebuildDictionaryFromChunks();
        void rebuildChunksFromDictionary();

        static void addToLog(const string &_s) ;
        void reconstructWordIts();
        void compact();
        void fixDictionaryHoles();

        // InvertedIndex.h (внутри class InvertedIndex, в private-секции)

        void applyBatchInStrand(PostingBatch batch);
        void processBatch(const PostingBatch& batch);

        friend class boost::serialization::access;

        template<class Archive>
        void save(Archive & ar, const unsigned int version)  const {
            std::lock_guard<std::mutex> lock(mapMutex);
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
        explicit InvertedIndex(boost::asio::thread_pool& cpu_pool, boost::asio::io_context& io_commit);
        bool enqueueFileUpdate(const std::wstring& path);
        bool enqueueFileDeletion(const std::wstring& path);
        ~InvertedIndex();
        void saveIndex() const;
    };

}

