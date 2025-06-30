//
// Created by user on 01.02.2023.
//
#include <boost/asio/post.hpp>
#include <windows.h>
#include <iostream>
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
    // ---------- после завершения пересканирования ----------
    {
        std::ostringstream oss;
        oss << "updateDocumentBase() → completed successfully; "
            << "dictionary="   << dictionary.size()    // уникальных слов
            << ", wordRefs="   << wordRefs.size()      // файлов с posting'ами
            << ", RAM="        << std::fixed << std::setprecision(1)
            << process_memory() / (1024.0 * 1024) << " MB";
        addToLog(oss.str());
    }
// --------------------------------------------------------


}


void inverted_index::InvertedIndex::addToDictionary(const setLastWriteTimeFiles& ind, size_t _threadCount) {
/** Индексирование файлов осуществляется в пуле потоков, количество потоков в пуле опеределяется
 * @param _threadCount. - если параметр пуст то выбирается по количеству ядер процессора,
 * если параметр больше количества файлов, то приравнивается количеству файлов. */
/*
    size_t threadCount = _threadCount ? _threadCount : thread::hardware_concurrency();
    threadCount = docPaths.size() < threadCount ? docPaths.size() : threadCount;

    auto oneInd = [this](size_t _hashFile)
    {
        fileIndexing(_hashFile);
    };

*/
   // thread_pool pool(threadCount);

    std::atomic<size_t> counter = ind.size();
    std::promise<void> all_done;
    auto all_done_future = all_done.get_future();
    std::cout << "start add tusk in pool" << std::endl;
    for(auto i:ind) {
        auto hash_file = i.first;
        boost::asio::post(pool_, [this, hash_file, &counter, &all_done]() {
            std::cout << "tusk run" << hash_file << std::endl;
            fileIndexing(hash_file);
            if (--counter == 0) {
                all_done.set_value();
            }
        });
    }
    if (ind.empty())
        all_done.set_value();

    all_done_future.wait(); // <-- Здесь поток main заблокируется, пока все задачи не выполнятся


/*
    for(auto i:ind)
        pool.add_task(oneInd,i.first);

    pool.wait_all();*/
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

    for(const auto& [word, cnt]:freqWordFile)
        {

            uint32_t wid = wordIds.getId(word);

            // 2. Увеличиваем vector, если нужно.
            if (wid >= dictionary.size())
                dictionary.resize(wid + 1);

            // 4. Быстрая ссылка «какие слова есть в этом файле».
            wordRefs[_fileHash].push_back(wid);
            dictionary[wid][_fileHash] += cnt;

            // 4. ссылка «слова файла» — добавляем Wid, если его ещё нет
            auto& refVec = wordRefs[_fileHash];
            if (refVec.empty() || refVec.back() != wid)   // простая защита от дубля
                refVec.push_back(wid);


        //    freqDictionary[word].insert(make_pair(_fileHash, cnt));
        //    wordIts[_fileHash].push_back(freqDictionary.find(word));
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

PostingList inverted_index::InvertedIndex::getWordCount(const string& _s) {
    /** Функция возвращающая @param mapEntry - обьект типа map<size_t,size_t>,
      * где first - индекс файла, а second - количество сколько раз в это файле встрчается
      * поисковое слово (ключ карты map @param freqDictionary) */

    std::lock_guard<std::mutex> lock(mapMutex);

    if (const auto* post = tryGetPosting(_s))
        return *post;                     // копия posting-листа

/*
    if(auto f = freqDictionary.find(_s); f != freqDictionary.end())
        return freqDictionary.at(_s);
    else
        return mapEntry {};
*/
return {};
}
/*
void inverted_index::InvertedIndex::delFromDictionary(const setLastWriteTimeFiles& del)
{
    std::lock_guard<std::mutex> lock(mapMutex);

    for (const auto& [file_name, file_time] : del)
    {

        std::vector<std::string> wordsToErase;          // Шаг 1: список слов, которые «опустеют»

        for (auto wordIt : wordIts[file_name])             // safe: копия итератора
        {
            if (wordIt == freqDictionary.end())         // защита от случайного dangling-а
                continue;

            auto &entry = wordIt->second;               // mapEntry
            entry.erase(file_name);

            if (entry.empty())
                wordsToErase.push_back(wordIt->first);  // пометим слово на удаление,
            //   но не трогаем map пока идём по вектору
        }

        for (const auto &w : wordsToErase)              // Шаг 2: удаляем слова одним
            freqDictionary.erase(w);                    //        проходом ПОСЛЕ цикла

        wordIts.erase(file_name);                          // Шаг 3: убираем вектор итераторов
    }
}
*/

void inverted_index::InvertedIndex::delFromDictionary(const setLastWriteTimeFiles& del)
{
    std::lock_guard<std::mutex> lock(mapMutex);      // защита индекса

    for (const auto& [fileId, _] : del)
    {
        /* ---------- 1. Найти все WordID, встречающиеся в файле ---------- */
        auto refIt = wordRefs.find(fileId);
        if (refIt == wordRefs.end())
            continue;                                // файл уже удалён ранее

        const std::vector<uint32_t>& widList = refIt->second;

        /* ---------- 2. Удаляем fileId из posting-листов ----------------- */
        std::vector<uint32_t> emptyWords;            // tombstones

        for (uint32_t wid : widList)
        {
            if (wid >= dictionary.size()) continue;  // защита от несоответствий

            auto& posting = dictionary[wid];
            posting.erase(fileId);                   // убрать запись (если была)

            if (posting.empty())                     // posting-лист опустел?
                emptyWords.push_back(wid);           // помечаем как «пустой»
        }

        /* ---------- 3. Чистим опустевшие posting-листы ------------------ */
        for (uint32_t wid : emptyWords)
            dictionary[wid].clear();                // tombstone (оставляем ячейку)

        /* ---------- 4. Удаляем ссылку fileId → Wid-ы -------------------- */
        wordRefs.erase(refIt);


        /* ---------- 5. СТАРЫЕ СТРУКТУРЫ (пока не используем) ------------ */
        /*
        for (auto wordIt : wordIts[fileId])
        {
            auto& entry = wordIt->second;
            entry.erase(fileId);
            if (entry.empty())
                freqDictionary.erase(wordIt->first);
        }
        wordIts.erase(fileId);
        */
    }
}



void inverted_index::InvertedIndex::dictonaryToLog() const {
  //  for(const auto& i:freqDictionary)
  //      addToLog(i.first + "\t" + std::to_string(i.second.size()));
}
/*
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
*/


void inverted_index::InvertedIndex::reconstructWordIts()
{
    /*  Новый смысл:
        восстанавливаем  wordRefs  (fileId → [WordID])
        пробегая по  dictionary   (WordID → posting-list)
    */

    wordRefs.clear();                         // прежний wordIts заменён на wordRefs

    for (uint32_t wid = 0; wid < dictionary.size(); ++wid)
    {
        const auto& posting = dictionary[wid];
        if (posting.empty()) continue;        // tombstone или пустой слот

        for (const auto& [docId, cnt] : posting)
            wordRefs[docId].push_back(wid);   // файл ↦ WordID
    }
}


void inverted_index::InvertedIndex::saveIndex() {
    std::ofstream ofs("inverted_index3.dat", std::ios::binary);
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

inverted_index::InvertedIndex::InvertedIndex(boost::asio::thread_pool& _pool) : pool_{_pool} {
    {
        // Проверка наличия файла с индексом
        if (std::filesystem::exists("inverted_index3.dat")) {
            // Если файл существует, загружаем данные
            std::ifstream ifs("inverted_index3.dat", std::ios::binary);
            if (ifs.is_open()) {
                boost::archive::binary_iarchive ia(ifs);
                ia >> *this; // Загрузка данных через Boost.Serialization
            }
        }
    }
}

void inverted_index::InvertedIndex::compact()
{
    std::lock_guard<std::mutex> lock(mapMutex);

    std::vector<PostingList> newDict;    // Было: vector<mapEntry>
    newDict.reserve(dictionary.size());

    std::unordered_map<uint32_t, uint32_t> remap; // oldWid → newWid

    for (uint32_t oldWid = 0; oldWid < dictionary.size(); ++oldWid)
    {
        auto& post = dictionary[oldWid];
        if (post.empty()) continue; // пропустить tombstone
        remap[oldWid] = static_cast<uint32_t>(newDict.size());
        newDict.push_back(std::move(post));
    }
    dictionary.swap(newDict);

    // Перестроить wordRefs по remap
    for (auto& [fileId, vec] : wordRefs)
        for (uint32_t& wid : vec)
            wid = remap.at(wid);   // at() чтобы бросить ошибку если что-то пошло не так
}

void inverted_index::InvertedIndex::safeEraseFile(size_t hash)
{
    /*  для каждой Wid-очереди стираем запись hash, если она есть
        (никаких erase по несуществующему индексу)                */
    for (auto& posting : dictionary)
    {
        if (!posting.empty())
            posting.erase(hash);
    }
    /* wordRefs может не содержать хэш: проверяем */
    auto it = wordRefs.find(hash);
    if (it != wordRefs.end())
        wordRefs.erase(it);
}
bool inverted_index::InvertedIndex::enqueueFileUpdate(const std::wstring& path)
{
    namespace fs = std::filesystem;

    /* ── проверяем файл ──────────────────────────── */
    if (!fs::exists(path) || !fs::is_regular_file(path))
    {
        std::wcout << L"[enqueueUpdate] skip (no file) "
                   << path << std::endl;
        return false;
    }

    fs::file_time_type ts   = fs::last_write_time(path);
    size_t             hash = std::hash<std::wstring>{}(path);

    {   /* ── критическая секция ─────────────────── */
        std::lock_guard<std::mutex> lock(mapMutex);

        if (!docPaths.needUpdate(hash, ts))
        {
            std::wcout << L"[enqueueUpdate] actual "
                       << path << std::endl;
            return false;                     // всё актуально
        }

        /* удалить старые записи (без краша) */
        safeEraseFile(hash);

        /* зафиксировать новое состояние */
        docPaths.upsert(hash, path, ts);

        std::wcout << L"[enqueueUpdate] queued "
                   << path << std::endl;
    }

    /* ── ставим пересчёт в пул ───────────────────── */
    boost::asio::post(pool_, [this, hash, p = std::wstring(path)]()
    {
        try {
        //    std::wcout << L"[fileIndexing] start  " << p << std::endl;
            fileIndexing(hash);
      //      std::wcout << L"[fileIndexing] done   " << p << std::endl;
        }
        catch (const std::exception& e) {
            std::wcout << L"[fileIndexing EXC] "
                       << p << L" : " << e.what() << std::endl;
        }
    });

    return true;
}

bool inverted_index::InvertedIndex::enqueueFileDeletion(const std::wstring& path)
{
    size_t hash = std::hash<std::wstring>{}(path);

    {   // ── быстрая проверка: есть ли запись о файле
        std::lock_guard<std::mutex> lock(mapMutex);
        if (docPaths.mapHashDocPaths.find(hash) == docPaths.mapHashDocPaths.end())
            return false;                       // индекс ничего о нём не знает
    }

    /*  Ставим тяжёлую работу в общий thread_pool.
        Вся логика выполняется под mapMutex,
        поэтому posting-листы и wordRefs остаются согласованными.  */
    boost::asio::post(pool_, [this, hash]()
    {
        std::lock_guard<std::mutex> lock(mapMutex);

        /* ---------- 1. Узнаём, какие Wid-ы есть в файле ---------- */
        auto refIt = wordRefs.find(hash);
        if (refIt == wordRefs.end())
            return;                             // файл уже удалён (race)

        const std::vector<uint32_t>& widList = refIt->second;

        /* ---------- 2. Чистим posting-листы ---------------------- */
        std::vector<uint32_t> emptyWords;       // tomstones

        for (uint32_t wid : widList)
        {
            if (wid >= dictionary.size()) continue;

            auto& posting = dictionary[wid];
            posting.erase(hash);                // убрать запись

            if (posting.empty())
                emptyWords.push_back(wid);      // запомним «пустышку»
        }

        for (uint32_t wid : emptyWords)
            dictionary[wid].clear();            // tombstone (ячейку оставляем)

        /* ---------- 3. Чистим wordRefs и docPaths ---------------- */
        wordRefs.erase(refIt);
        docPaths.erase(hash);

        /* ---------- 4. (опц.) перестройка кэша ------------------- */
        //  Если в дальнейшем потребуется rebuild — здесь же:
        //  reconstructWordIts();
    });

    return true;                                // задача успешно поставлена
}


