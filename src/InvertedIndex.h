//
// Created by user on 01.02.2023.
//
#pragma once
#include <boost/asio/thread_pool.hpp>
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
#include "WordID.h"
#include "PostingList.h"



#include "DocPaths.h"

namespace search_server
{
    class SearchServer;
    struct RelativeIndex;
}

namespace inverted_index {

    using namespace std;

    class hashFunction;
    typedef unordered_map<size_t, size_t> mapEntry;
    typedef map<size_t, vector<unordered_map<string,mapEntry>::iterator>> mapDictionaryIterators;


    class OEMtoUpper
    {

    public:
        inline static std::map<char,char> mapChar = {};
        OEMtoUpper(const std::string p)
        {
            std::ifstream file(p, std::ios::binary);

            if(!file.is_open())
                return;

            std::string s1;
            std::string s2;
            getline(file,s1);
            getline(file,s2);
            file.close();

            for(int i =0;i<33;i++)
                mapChar.insert(std::make_pair(s1[i],s2[i]));


        }
        static bool iS_not_a_Oem(char c)
        {
            static auto a_oem = mapChar.begin()->first;
            return c != a_oem;
        }
        static char getUpperCharOem(char c)
        {
            if(auto i = mapChar.find(c);i!=mapChar.end())
                return i->second;
            else
                return std::toupper(c);
        };
    };



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

        /** @param work для проверки выполнения в текущий момент времени переиндексации базы 'freqDictionary'.
            @param freqDictionary база индексов.
            @param docPaths пути индексируемых файлов.
            @param mapMutex мьютекс для разделенного доступа потоков к базе индексов 'freqDictionary', во время
            выполнения индексирования базы.*
            @param logMutex мьютекс для разделения доступа между потоками к файлу 'logFile'.
            @param logFile файл для хранения информации о работе сервера. */


        // В public-секции InvertedIndex
        const PostingList* tryGetPosting(const std::string& w) const
        {
            uint32_t wid;
            if (!wordIds.tryGet(w, wid))
                return nullptr;
            if (wid >= dictionary.size())
                return nullptr;
            const PostingList* post = &dictionary[wid];
            return post->empty() ? nullptr : post;
        }




        atomic<bool> work{};
        DocPaths docPaths;
        mutable mutex mapMutex;
        mutable mutex logMutex;
        mutable ofstream logFile;
       // unordered_map<string, mapEntry> freqDictionary;
     //   mapDictionaryIterators wordIts;

        using mapEntry = std::unordered_map<size_t, size_t>;   // fileId → count
      //  std::vector<mapEntry> dictionary;    // НОВЫЙ контейнер (WordID → posting list)
        WordIdManager wordIds;               // то, что добавили на шаге 1
        std::unordered_map<size_t, std::vector<uint32_t>> wordRefs;
        std::vector<PostingList> dictionary;

        boost::asio::io_context& io_;

        friend class search_server::SearchServer;
        friend class search_server::RelativeIndex;

        /** @param fileIndexing функция индексирования одного файла.
            @param allFilesIndexing функция индексирования всех файлов из mapHashDocPaths.
            @param addToLog функция добавление в 'logFile' инфрормации о работе сервера. */

        void fileIndexing(size_t _fileHash);
        void safeEraseFile(size_t hash);
        void addToDictionary(const setLastWriteTimeFiles& ind, std::function<void()> on_done);
        void delFromDictionary(const setLastWriteTimeFiles& del);
        void addToLog(const string &_s) const;
        void reconstructWordIts();
        void compact();
        friend class boost::serialization::access;

        template<class Archive>
        void save(Archive & ar, const unsigned int version) const {
            std::lock_guard<std::mutex> lock(mapMutex);
          //  ar & freqDictionary;
            ar & dictionary;
            //   ar & wordIts;
            ar & wordIds;
            ar & docPaths;
        }

        template<class Archive>
        void load(Archive & ar, const unsigned int version) {
            std::lock_guard<std::mutex> lock(mapMutex);
            //ar & freqDictionary;
            ar & dictionary;
            //  ar & wordIts;
            ar & wordIds;
            ar & docPaths;
            reconstructWordIts();
        }

        BOOST_SERIALIZATION_SPLIT_MEMBER()

    public:

        /** @param setDocPaths для установки путей файлов подлежащих индексации.
            @param updateDocumentBase функция процесс обновления базы индексов.
            @param addToLog функция добавление в 'logFile' инфрормации о работе сервера.
            @param getWordCount функция возвращающая mapEntry - обьект типа map<size_t,size_t>
            где first - это индекс файла, а second - количество сколько раз 'word'
            содержится в файле с индексом first - функция используется только для тестирования */

        void updateDocumentBase(const std::list<wstring> &vecPaths, size_t threadCount = 0);
        PostingList getWordCount(const string& word);
        void dictonaryToLog() const;
        InvertedIndex(boost::asio::io_context& _io);
        bool enqueueFileUpdate(const std::wstring& path);
        bool enqueueFileDeletion(const std::wstring& path);
        ~InvertedIndex();
        void saveIndex();
    };

}

