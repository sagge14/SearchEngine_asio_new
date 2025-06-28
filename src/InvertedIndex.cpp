//
// Created by user on 01.02.2023.
//
#include <windows.h>
#include <iostream>
#include <utility>
#include <list>
#include <ranges>
#include <execution>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <thread>
#include <atomic>
#include "InvertedIndex.h"
#include <codecvt>
#include <boost/algorithm/string.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/unordered_set.hpp>
#include <boost/serialization/map.hpp>



   // return conv::strconverter.to_bytes(wstr);



   // return conv::strconverter.from_bytes(str);



void inverted_index::InvertedIndex::updateDocumentBase(const std::list<wstring> &vecPaths, size_t threadCount)
{
    addToLog("updateDocumentBase() → start");
    work = true;

    addToLog("updateDocumentBase() → calling docPaths.getUpdate()");
    setLastWriteTimeFiles ind, del;
    try {
        std::tie(ind, del) = docPaths.getUpdate(vecPaths);
        addToLog("updateDocumentBase() → getUpdate() completed: "
                 + std::to_string(ind.size()) + " files to add/update, "
                 + std::to_string(del.size()) + " files to delete");
    } catch (const std::exception& e) {
        addToLog("updateDocumentBase() → exception in getUpdate(): " + std::string(e.what()));
        work = false;
        throw;
    } catch (...) {
        addToLog("updateDocumentBase() → unknown exception in getUpdate()");
        work = false;
        throw;
    }

    try {
        addToLog("updateDocumentBase() → calling delFromDictionary()");
        delFromDictionary(del);
        addToLog("updateDocumentBase() → delFromDictionary() done");
    } catch (const std::exception& e) {
        addToLog("updateDocumentBase() → exception in delFromDictionary(): " + std::string(e.what()));
        work = false;
        throw;
    }

    try {
        addToLog("updateDocumentBase() → calling addToDictionary() with threadCount = " + std::to_string(threadCount));
        addToDictionary(ind, threadCount);
        addToLog("updateDocumentBase() → addToDictionary() done");
    } catch (const std::exception& e) {
        addToLog("updateDocumentBase() → exception in addToDictionary(): " + std::string(e.what()));
        work = false;
        throw;
    }

    work = false;
    addToLog("updateDocumentBase() → completed successfully");
}


void inverted_index::InvertedIndex::addToDictionary(const setLastWriteTimeFiles& ind, size_t _threadCount) {
/** Индексирование файлов осуществляется в пуле потоков, количество потоков в пуле опеределяется
 * @param _threadCount. - если параметр пуст то выбирается по количеству ядер процессора,
 * если параметр больше количества файлов, то приравнивается количеству файлов. */

    size_t threadCount = _threadCount ? _threadCount : thread::hardware_concurrency();
    threadCount = docPaths.size() < threadCount ? docPaths.size() : threadCount;

    auto oneInd = [this](size_t _hashFile)
    {
        fileIndexing(_hashFile);
    };

    thread_pool pool(threadCount);

    for(auto i:ind)
        pool.add_task(oneInd,i.first);

    pool.wait_all();
}

void inverted_index::InvertedIndex::fileIndexing(size_t _fileHash)
{
    /** Если @param  TEST_MODE = true, то @param docPaths используется не как
     * база путей индексируемых файлов, а как база самих файлов (строк)
     * Из текста удаляются все символы кроме букв и цифр,
     * все буквы приводятся к нижнему регистру
     * @param freqWordFile - карта частоты слов файла */

    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> strconverter;

    #ifndef TEST_MODE
    ifstream file;
    map<string, size_t> freqWordFile;
    std::vector<char> data;

    file = ifstream(docPaths.at(_fileHash).c_str());

    if(!file.is_open())
    {
        addToLog("File " + strconverter.to_bytes(docPaths.at(_fileHash)) + " - not found!");
        return;
    }

    #endif

    #ifdef TEST_MODE
    string text(docPaths.at(_fileHash));
    #endif



    file.unsetf(std::ios::skipws);

    size_t fileSize;
    file.seekg(0, std::ios::end);
    fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    data.reserve(fileSize );
    try
    {
        data.insert(data.begin(),
                    std::istream_iterator<BYTE>(file),
                    std::istream_iterator<BYTE>());
        file.close();
    }
    catch(...)
    {
        file.close();
        return;
    }


    std::stringstream ss;
    std::copy(data.begin(), data.end(), std::ostream_iterator<char> (ss));


    auto wordRange =  std::ranges::istream_view<string>(ss) | std::views::transform ([](const string& text)
            {         
                auto s = text;

                boost::algorithm::trim_if(s,[](auto c){return OEMtoUpper::iS_not_a_Oem(c) && ispunct(c);});

                for(auto& c:s)
                    c = OEMtoUpper::getUpperCharOem(c);

                return s;
            });

    std::ranges::for_each(wordRange, [&freqWordFile](auto&& word)
                          {


                                if(auto item = freqWordFile.find(word); item != freqWordFile.end())
                                  item->second++;
                              else
                                  freqWordFile[word] = 1;
                          }
    );



    if(freqWordFile.empty())
        return;

    lock_guard<mutex> myLock(mapMutex);

    for(const auto& item:freqWordFile)
        {
            freqDictionary[item.first].insert(make_pair(_fileHash, freqWordFile.at(item.first)));
            wordIts[_fileHash].push_back(freqDictionary.find(item.first));
        }
}

void inverted_index::InvertedIndex::addToLog(const string& _s) const {

    /**
    Функция для записи информации в лог-файл*/

    char dataTime[20];
    time_t now = time(nullptr);
    strftime( dataTime, sizeof(dataTime),"%H:%M:%S %Y-%m-%d", localtime(&now));

    lock_guard<mutex> myLock(logMutex);

    logFile.open("log.ini", ios::app);
    logFile << "[" << dataTime << "] " << _s << endl;
    logFile.close();

}

inverted_index::mapEntry inverted_index::InvertedIndex::getWordCount(const string& _s) {
    /** Функция возвращающая @param mapEntry - обьект типа map<size_t,size_t>,
      * где first - индекс файла, а second - количество сколько раз в это файле встрчается
      * поисковое слово (ключ карты map @param freqDictionary) */

    if(auto f = freqDictionary.find(_s); f != freqDictionary.end())
        return freqDictionary.at(_s);
    else
        return mapEntry {};

}

void inverted_index::InvertedIndex::delFromDictionary(const setLastWriteTimeFiles& _del) {

    for(const auto& f:_del)
    {
        for(auto& i:wordIts[f.first])
        {
            i->second.erase(f.first);
            if(i->second.size() == 0)
                freqDictionary.erase(i->first);
        }
        wordIts.erase(f.first);
    }
}

void inverted_index::InvertedIndex::dictonaryToLog() const {
    for(const auto& i:freqDictionary)
        addToLog(i.first + "\t" + std::to_string(i.second.size()));
}

void inverted_index::InvertedIndex::reconstructWordIts() {
    wordIts.clear();
    for (const auto& [word, docMap] : freqDictionary) {
        auto wordIter = freqDictionary.find(word);
        if (wordIter != freqDictionary.end()) {
            for (const auto& [docId, count] : docMap) {
                wordIts[docId].push_back(wordIter);
            }
        }
    }
}

void inverted_index::InvertedIndex::saveIndex() {
    std::ofstream ofs("inverted_index.dat", std::ios::binary);
    if (ofs.is_open()) {
        boost::archive::binary_oarchive oa(ofs);
        oa << *this;
        ofs.close();
    } else {
        addToLog("Failed to open file for saving index.");
    }
}

inverted_index::InvertedIndex::~InvertedIndex() {
    saveIndex();
}

inverted_index::InvertedIndex::InvertedIndex() {
    {
        // Проверка наличия файла с индексом
        if (std::filesystem::exists("inverted_index.dat")) {
            // Если файл существует, загружаем данные
            std::ifstream ifs("inverted_index.dat", std::ios::binary);
            if (ifs.is_open()) {
                boost::archive::binary_iarchive ia(ifs);
                ia >> *this; // Загрузка данных через Boost.Serialization
            }
        }
    }
}
