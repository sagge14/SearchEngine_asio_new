//
// Created by user on 01.02.2023.
//
#include "md5/md5Hasher.h"
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

#include "SerializationHelpers.h"

#include <boost/serialization/set.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
//#include <boost/serialization/tuple.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

   // return conv::strconverter.to_bytes(wstr);

   // return conv::strconverter.from_bytes(str);

void inverted_index::InvertedIndex::updateDocumentBase(const std::list<wstring> &vecPaths, size_t threadCount)
{
    /** Очищаем старую базу индексов формируем новую.
     во время выполнения переиндексирования устанавливаем  @param work = true. */
    work = true;
    addToLog("Files count " + std::to_string(docPaths.size()) + " - ");

    auto [ind,del] = docPaths.getUpdate(vecPaths);

    addToLog("Files for ind " + std::to_string(ind.size()) + " - ");
    addToLog("Files for del " + std::to_string(del.size()) + " - ");

    addToDictionary(ind, threadCount);

    for(const auto& i: del) {
        delFromDictionary(i);
        docPaths.erase(i);
    }

    work = false;
}

void inverted_index::InvertedIndex::addToDictionary(const std::vector<size_t>& ind, size_t _threadCount) {
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
        pool.add_task(oneInd,i);

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


    for(auto& c:data)
        c = OEMtoUpper::getUpperCharOem(c);

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

    if(docPaths.reIndex(_fileHash))
        delFromDictionary(_fileHash);

    for(const auto& [word, count]:freqWordFile)
        {
            freqDictionary[word].insert(make_pair(_fileHash, count));
            wordIts[_fileHash].push_back(freqDictionary.find(word));
        }

    docPaths.addMd5HashFile(_fileHash);
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

void inverted_index::InvertedIndex::delFromDictionary(size_t _path_hash) {


        for(auto& i:wordIts[_path_hash])
        {
            i->second.erase(_path_hash);
            if(i->second.size() == 0)
                freqDictionary.erase(i->first);
        }
        wordIts.erase(_path_hash);

}

void inverted_index::InvertedIndex::dictonaryToLog() const {
    for(const auto& i:freqDictionary)
        addToLog(i.first + "\t" + std::to_string(i.second.size()));
}

inverted_index::InvertedIndex::InvertedIndex() {
    std::ifstream ifs("inverted_index.dat", std::ios::binary);
    if (ifs.is_open()) {
        try {
            boost::archive::binary_iarchive ia(ifs);
            ia >> *this;
            ifs.close();

        } catch (const std::exception &e) {
            ifs.close();
            freqDictionary.clear();
            wordIts.clear();
            docPaths = DocPaths();
            // Инициализируйте vecPaths вашими данными
        }
    }
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

std::wstring inverted_index::DocPaths::at(size_t _hashFile) const {
    /** Функция возвращающая полный путь к индексируемую файлу по его хэшу*/
    return mapHashDocPaths.at(_hashFile);
}

size_t inverted_index::DocPaths::size() const {
    /** Функция возвращающая количество индексируемых файлов*/
    return mapHashDocPaths.size();
}


std::tuple<std::vector<size_t>, std::vector<size_t>>  inverted_index::DocPaths::getUpdate(const std::list<wstring> &vecDoc) {

    /** Функция возвращающая количество индексируемых файлов*/

    std::vector<size_t> newFilesSizeT, forInd, forDel;
    std::set<size_t> newFilesSet;

    for(const auto& path:vecDoc)
    {
        size_t pathHash = std::hash<wstring>()(path);
        newFilesSet.insert(pathHash);

        try {

            mapHashDocPaths[pathHash] = path;

            auto md5hash = md5Hasher::calculateMD5(path);
            auto it = mapMd5HashFiles.find(pathHash);

            if(it == mapMd5HashFiles.end() || (it != mapMd5HashFiles.end() && it->second != md5hash)) {
                forInd.push_back(pathHash);
            }

        }
        catch(...)
        {
            continue;
        }
    }
    for(const auto& p:mapHashDocPaths)
    {
        if(newFilesSet.find(p.first) == newFilesSet.end())
            forDel.push_back(p.first);
    }

    return {forInd, forDel};
}

bool inverted_index::DocPaths::reIndex(size_t _path_hash) const {
    return mapMd5HashFiles.find(_path_hash) != mapMd5HashFiles.end();
}

void inverted_index::DocPaths::addMd5HashFile(size_t _hash_path) {
    mapMd5HashFiles[_hash_path] = md5Hasher::calculateMD5(mapHashDocPaths.at(_hash_path));
}

void inverted_index::DocPaths::erase(size_t _path_hash) {
    mapHashDocPaths.erase(_path_hash);
}
