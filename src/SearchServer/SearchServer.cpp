//

// Created by user on 02.02.2023.
//
#include "SearchServer.h"
#include <functional>
#include <iostream>
#include <string>
#include "MyUtils/Encoding.h"
#include "MyUtils/FileScanner.h"

namespace {

inverted_index::IndexStorageConfig makeIndexStorageConfig(
    const search_server::Settings& settings)
{
    inverted_index::IndexStorageConfig config;
    config.kind = inverted_index::IndexSerializationKind::SQLite;
    config.path = "inverted_index.sqlite";
    config.sqliteMirrorFlushIntervalSec =
        settings.sqliteMirrorFlushIntervalSec;
    config.sqliteMirrorMaxPendingOps = settings.sqliteMirrorMaxPendingOps;
    config.sqliteLoadThreads = settings.sqliteLoadThreads;
    config.sqlitePrecountPostings = settings.sqlitePrecountPostings;
    config.fullIndexStrategy = settings.fullIndexStrategy;
    config.documentCatalogStorage = settings.documentCatalogStorage;
    config.batchReaderThreads =
        static_cast<std::size_t>(settings.batchReaderThreads);
    config.batchIndexerThreads =
        static_cast<std::size_t>(settings.batchIndexerThreads);
    config.batchQueueMemoryBytes =
        static_cast<std::size_t>(settings.batchQueueMemoryMb) *
        1024u * 1024u;
    return config;
}

} // namespace

FileEvent merge(FileEvent old, FileEvent neu)
{
    if (neu == FileEvent::Removed || neu == FileEvent::RenamedOld)
        return neu;                         // удаление всегда победит

    if (neu == FileEvent::RenamedNew)       // «новое имя» = Added
        neu = FileEvent::Added;

    if (old == FileEvent::Removed)          // уже помечен удалённым
        return old;

    if (old == FileEvent::Added && neu == FileEvent::Modified)
        return old;                         // Added+Modified == Added

    return neu;                             // иначе последнее
}


search_server::setFileInd search_server::SearchServer::intersectionSetFiles(const set<string> &request) const {


    if(request.empty())
        return {};

    std::vector<std::optional<QueryPostingSet>> postings;
    postings.reserve(request.size());
    for (const auto& word : request)
    {
        auto post = index->getPostingCopyByWord(word);
        if (post)
        {
            QueryPostingSet files;
            for (const auto& [fileId, _] : *post) {
                files.insert(fileId);
            }
            postings.emplace_back(std::move(files));
        }
        else
        {
            postings.emplace_back(std::nullopt);
        }
    }
    return combineQueryPostings(settings.queryWordMatch, postings);
}

listAnswer search_server::SearchServer::getAnswer(const string& _request) const {
    /** Сначала с помощью атомарной булевой переменной @param work проверяем не осущетвляется ли в данный
     * момент времени обновление базы индексов (переиндексация файлов, заданных в настройках сервера).
     * Если работа по переиндексации выполняется то выдаем предупреждающее собщение и раз в 2 секунды
     * проверяем закончилась ли работа по переиндексации и далее выполняем запрос.
     * Для выполнения запроса @param _request сначала получаем с помощью функции @param getUniqWords
     * std::set  состоящий из уникальных слов запроса (разделитель пробел),
     * содержащих только буквы и цифры.*
     * Далее с помощью функции @param intersectionSetFiles находятся все файлы удовлетворяющие поисковому запросу,
     * для каждого из них расчитывается абсолютная релевантность в процессе составления списка @param Results
     * обьектов @param RelativeIndex - перед составлением списка статическая переменная @param max -
     * максимальная абсолютная релевантность - обнуляется.
     * Далее список @param Results сортируется по убыванию абсолютной релевантности и из него формируется
     * окончательный ответ @param out типа @param listAnswer  в виде списка пар идентификаторов документа (путь
     * или индекс в зависимости от настроек сервера) и относительной релевантности.
     * Размером списока ответов ограничен @param maxResponse.
     * */

    OperationGuard operation(*this);
    if (!operation)
        return {};

    work = true;
    struct SearchWorkReset final {
        std::atomic<bool>& value;
        const std::atomic<bool>& pendingUpdate;
        std::condition_variable_any& condition;
        ~SearchWorkReset()
        {
            value.store(false, std::memory_order_release);
            if (pendingUpdate.load(std::memory_order_acquire))
                condition.notify_one();
        }
    } searchWorkReset{work, must_start_update, cv_search_server};

    if(index->work)
        std::cout << "Update base is running, pls wait!!!" << endl;

    std::shared_lock<std::shared_mutex> lock{searchM};
    cv_search_server.wait(lock, [this] {return !update_is_running;});
    //updateM.lock();

    std::cout << "Request start !!!" << (std::thread::id) std::this_thread::get_id() << endl;

    set<std::string> request = getUniqWords(_request);

    list<RelativeIndex> Results;
    listAnswer out;

    RelativeIndex::max = 0;

    for(const auto& fileInd: intersectionSetFiles(request))
        Results.emplace_back(fileInd, request, index, settings.queryWordMatch);

    Results.sort();

    std::vector<const RelativeIndex*> selected;
    std::vector<uint32_t> selectedIds;
    const std::size_t responseLimit = settings.maxResponse > 0
        ? static_cast<std::size_t>(settings.maxResponse)
        : 0;
    selected.reserve(std::min(responseLimit, Results.size()));
    selectedIds.reserve(std::min(responseLimit, Results.size()));
    for (const auto& result : Results) {
        if (selected.size() >= responseLimit)
            break;
        selected.push_back(&result);
        selectedIds.push_back(result.fileId);
    }

    // Пути запрашиваются только для уже отсортированного top-N. SQLite
    // backend сам делит IN-запросы по SQLITE_LIMIT_VARIABLE_NUMBER.
    const auto documents = index->documentsByIds(selectedIds);
    for (std::size_t item = 0; item < selected.size(); ++item) {
        if (!documents[item]) {
            throw std::runtime_error(
                "document catalog has no row for search result doc_id=" +
                std::to_string(selectedIds[item]));
        }
        out.push_back(AnswerItem{
            encoding::wstring_to_utf8(documents[item]->path),
            selected[item]->getRelativeIndex(),
            documents[item]->deleted});
    }

 //   updateM.unlock();
    std::cout << "Request finish!!!" << endl;
    return out;
}

std::set<std::string> search_server::SearchServer::getUniqWords(const string& text) {
    /**
    Функция разбиения строки @param text на std::set слов.*/

   // transform(text.begin(), text.end(), text.begin(), [](char c){
  //      return isalnum(c) ? tolower(c) : (' '); });
    istringstream iss(text);
    std::string word;
    set<std::string> out;

    while(iss)
    {
        iss >> word;
        if (iss)
            out.insert(word);
    }

    return out;
}

listAnswers search_server::SearchServer::getAllAnswers(const vector<string>& requests) const {
    listAnswers out;
    for (const auto& request : requests)
        out.emplace_back(getAnswer(request), request);
    return out;
}

void search_server::SearchServer::updateDocumentBase(const std::vector<std::wstring> & _docPaths) {
    /**
    Запускаем обновление базы индексов, записываем в @param time сколько времени уйдет на индексацию. */

    time = inverted_index::perf_timer<chrono::milliseconds>::duration([this, &_docPaths]() {
        std::future<void> fut = this->index->updateDocumentBase(_docPaths);

        // Ждём завершения индексации в InvertedIndex; compact/save — в SearchServer::updateStep
        fut.get();
        }).count();

}


search_server::SearchServer::SearchServer(const Settings& _settings , boost::asio::thread_pool& cpu_pool, boost::asio::io_context& _io_commit) : time{},
index(), cpu_pool_(cpu_pool), io_commit(_io_commit)
{

    settings = (_settings);
    trustSettings();

    const inverted_index::IndexStorageConfig cfg =
        makeIndexStorageConfig(settings);

    index = new inverted_index::InvertedIndex(cpu_pool, io_commit, settings.maxParallelReaders, settings.fileIndexingTimeoutSec, cfg);

}

void search_server::SearchServer::trustSettings() const {
    if (settings.threadCount < 0)
        throw(myExp(ErrorCodes::THREADCOUNT));
}

bool search_server::SearchServer::checkHash(bool resetHash) const {
    /**
    Функция сравнения хешей очередного и последнего запроса*/

    static size_t hash{0};
    size_t newHash;

    if(resetHash)
    {
        hash = 0;
        return{};
    }

    string textRequest;
    std::ifstream jsonFileRequests("Requests.json");

    if(jsonFileRequests.is_open())
    {
        textRequest = std::string ((istreambuf_iterator<char>(jsonFileRequests)), (istreambuf_iterator<char>()));
        jsonFileRequests.close();
    }
    else
        return false;

    newHash = hashRequest(textRequest);
    auto check = newHash != hash;
    hash = newHash;

    return check;
}

void search_server::addToLog(const string &s) {
    /**
    Функция для записи информации работе сервера в лог-файл*/
    static std::mutex logMutex;
    static ofstream logFile;

    char dataTime[20];
    time_t now = std::time(nullptr);
    strftime( dataTime, sizeof(dataTime),"%H:%M:%S %Y-%m-%d", localtime(&now));

    std::lock_guard<std::mutex> myLock(logMutex);
    logFile.open("server_log.log", ios::app);
    logFile << "[" << dataTime << "] " << s + ";" << endl;
    logFile.close();

  //  std::cout << s << std::endl;
}

search_server::SearchServer::~SearchServer() {
    /**
    Деструктор класса*/

    try {
        stop();
    }
    catch (const std::exception& exception) {
        addToLog(
            std::string("SearchServer destructor stop failed: ") +
            exception.what());
    }
    catch (...) {
        addToLog("SearchServer destructor stop failed: unknown exception");
    }
    delete index;
    index = nullptr;

}

void search_server::SearchServer::showSettings() const {
    /**
    Для отображения текущих настроек сервера*/
    settings.show();
}

size_t search_server::SearchServer::getTimeOfUpdate() const {
    /**
    Для получения длительности последнего обновления базы индексов*/
    return time;
}

void search_server::SearchServer::dictionaryToLog() const {
    index->dictonaryToLog();
}

void search_server::SearchServer::flushUpdateAndSaveDictionary() {
    if (stopping_.load(std::memory_order_acquire) || update_is_running)
        return;

    bool expected = false;
    if (!must_start_update.compare_exchange_strong(expected, true))
        return;

    if (!beginOperation()) {
        must_start_update = false;
        return;
    }

    try {
        boost::asio::post(cpu_pool_, [this] {
            try {
                updateStep();
            } catch (const std::exception& e) {
                addToLog(std::string("Background update failed: ") + e.what());
            } catch (...) {
                addToLog("Background update failed: unknown exception");
            }
            must_start_update = false;
            finishOperation();
        });
    } catch (...) {
        must_start_update = false;
        finishOperation();
        throw;
    }
}




[[maybe_unused]] bool search_server::SearchServer::getIsUpdateRunning() const {
    std::cout << " get index wordT! - " << index->work << " "  <<true << endl;
    return index->work;
}

search_server::RelativeIndex::RelativeIndex(
    size_t _fileInd,
    const set<string>& _request,
    const inverted_index::InvertedIndex* _index,
    QueryWordMatch queryWordMatch)

{
    /**
    Т.к. сервер может работать в двух режимах: точного поиска и обычного, для которого не обязательно все слова из запроса
     должны одержаться в файле-результате, то во втором случае перед вычисление количества сколько раз слово
     встречается в файле нужно проверить есть ли вообще это слово в словаре @param freqDictionary и есть ли
     это слово в самом документе с индексом @param fileInd - эту проверку осуществляет функция @param checkWordAndFileInd.
     @param sum статическое поле класса, хранит максимальную абсолютную релевантность файла-результата, используется для
     вычисления относительной релевантности.
     */

    fileId = static_cast<uint32_t>(_fileInd);

    auto checkWordAndFileInd = [_index](const std::string& word)
    {
        return _index->getPostingCopyByWord(word).has_value();
    };

    for (const auto& word : _request)
    {
        if (queryWordMatch == QueryWordMatch::All ||
            checkWordAndFileInd(word))
        {
            if (auto post = _index->getPostingCopyByWord(word); post)
            {
                if (const uint16_t* frequency = post->find(_fileInd))
                    sum += *frequency;
            }
        }
    }

    if(sum > max)
        max = sum;
}

void search_server::SearchServer::myExp::show() const {
    /**
    Сообщения о возможных ошибках в настройках сервера*/
    if(codeExp == ErrorCodes::NAME)
        cout << "Server: settings error!" << endl;
    if(codeExp == ErrorCodes::THREADCOUNT)
        cout << "Server: thread count error!" << endl;
    if(codeExp == ErrorCodes::WRONGDIRRECTORY)
        cout << "Server: directory '" << dir << "' is not exist!" << endl;
    if(codeExp == ErrorCodes::NOTFILESTOINDEX)
        cout << "Server: no files to index!" << endl;

}

void search_server::Settings::show() const
{
    std::cout << "--- Server information ---" << std::endl;
    std::cout << std::endl;
    std::cout << "Number of maximum responses:\t" << maxResponse << std::endl;
    std::cout << "Thread count:\t\t\t";
    if(threadCount)
        std::cout << threadCount << std::endl;
    else
        std::cout << std::thread::hardware_concurrency() << std::endl;
    std::cout << "Index database update period:\t" << indTime << " seconds" << std::endl;
    std::cout << "Scan on startup:\t\t" << std::boolalpha << scanOnStartup << std::endl;
    std::cout << "Asio port:\t\t\t" << port << std::endl;
    std::cout << "Query word match:\t\t"
              << search_server::toString(queryWordMatch) << std::endl;
    std::cout << "Hide console window:\t\t" << std::boolalpha << hideConsoleWindow << std::endl << std::endl;
    std::cout << "Full index strategy:\t\t"
              << inverted_index::toString(fullIndexStrategy) << std::endl;
    std::cout << "Document catalog storage:\t"
              << inverted_index::toString(documentCatalogStorage) << std::endl;
    std::cout << "Batch reader threads:\t\t" << batchReaderThreads << std::endl;
    std::cout << "Batch indexer threads:\t\t" << batchIndexerThreads << std::endl;
    std::cout << "Batch queue memory:\t\t" << batchQueueMemoryMb << " MiB" << std::endl;
}

search_server::Settings *search_server::Settings::getSettings() {

    if(!settings)
        settings = new Settings();

    return settings;

}


void search_server::logState(const std::string& where,
              bool update_is_running,
              bool work,
              bool index_work)
{
    addToLog(
            where +
            " | update_is_running=" + std::to_string(update_is_running) +
            " work=" + std::to_string(work) +
            " index->work=" + std::to_string(index_work)
    );
}




void search_server::SearchServer::updateStep()
{
    OperationGuard operation(*this);
    if (!operation) {
        must_start_update = false;
        return;
    }

    addToLog("updateStep() ENTER");

    std::unique_lock<std::mutex> lock2{updateM};

    logState("before guards", update_is_running, work, index != nullptr && index->work.load());

    if (update_is_running || (index && index->work))
    {
        addToLog("updateStep() EXIT: already running");
        return;
    }

    addToLog("updateStep() → started");

    if (index == nullptr)
    {
        const inverted_index::IndexStorageConfig cfg =
            makeIndexStorageConfig(settings);
        index = new inverted_index::InvertedIndex(
            cpu_pool_,
            io_commit,
            settings.maxParallelReaders,
            settings.fileIndexingTimeoutSec,
            cfg);
        addToLog("updateStep() → index created");
    }

    /* ---------- СКАНИРОВАНИЕ ---------- */
    addToLog("updateStep() → scan start");

    const std::vector<std::wstring> scannedPaths = FileScanner::scanDirectories(
            settings.indexRoots,
            file_extension_contract::Selection{
                settings.indexedExtensions,
                settings.includeExtensionlessFiles},
            settings.excludedSubtrees
    );

    addToLog("updateStep() → scan done, files=" +
             std::to_string(scannedPaths.size()));

    /* ---------- ОЖИДАНИЕ ПОИСКА ---------- */
    addToLog("updateStep() → wait search stop");

    // Keep the exclusive search gate through publication and persistence of
    // the new snapshot. Existing searches finish first; new ones wait.
    std::unique_lock<std::shared_mutex> searchLock{searchM};
    bool ok = cv_search_server.wait_for(
            searchLock,
            std::chrono::seconds(10),
            [this] { return !work; }
    );

    addToLog(std::string("updateStep() → wait done, result=") +
             (ok ? "OK" : "TIMEOUT") +
             " work=" + std::to_string(work));

    /* ---------- СТАРТ ОБНОВЛЕНИЯ ---------- */
    update_is_running = true;
    must_start_update = false;

    logState("before updateDocumentBase", update_is_running, work, index->work);


    std::future<void> fut = std::async(
            std::launch::async,
            &search_server::SearchServer::updateDocumentBase,
            this,
            std::cref(scannedPaths)
    );

    addToLog("updateStep() → updateDocumentBase started");

    /* ---------- ОЖИДАНИЕ ИНДЕКСАЦИИ ---------- */
    addToLog("updateStep() → waiting updateDocumentBase");

    try {
        fut.get();
    }
    catch (const std::exception& exception) {
        update_is_running = false;
        must_start_update = false;
        cv_search_server.notify_all();
        addToLog(
            std::string("updateStep() → updateDocumentBase FAILED: ") +
            exception.what());
        throw;
    }
    catch (...) {
        update_is_running = false;
        must_start_update = false;
        cv_search_server.notify_all();
        addToLog("updateStep() → updateDocumentBase FAILED: unknown exception");
        throw;
    }

    addToLog("updateStep() → updateDocumentBase finished");

    /* ---------- ФИНАЛ ---------- */
    index->compact(settings.compactThresholdPercent);
    index->waitForIdle();
    addToLog("updateStep() → compact done");

    addToLog("updateStep() → saveIndex");
    index->saveIndex();

    time = getTimeOfUpdate();

    // Получаем статистику словаря (используем wordIds.size() вместо dictionary.size())
    auto stats = index->getStats();
    addToLog("Index database update completed! " +
             std::to_string(stats.totalFiles) + " files, " +
             std::to_string(stats.uniqueWords) +
             " unique words, " +
             std::to_string(stats.totalPostings) + " postings. Time " +
             std::to_string(time) + " sec");

    update_is_running = false;
    must_start_update = false;

    cv_search_server.notify_all();

    logState("updateStep() EXIT", update_is_running, work, index->work);
}




void search_server::SearchServer::addFileToIndex(const std::wstring& path) {
    OperationGuard operation(*this);
    if(!operation || !index)
        return;

    index->enqueueFileUpdate(path);
}

void search_server::SearchServer::removeFileFromIndex(const std::wstring &path) {
    OperationGuard operation(*this);
    if(!operation || !index)
        return;
    index->enqueueFileDeletion(path);
}

search_server::SearchServer::OperationGuard::OperationGuard(
    SearchServer& server)
    : server_(&server), active_(server.beginOperation())
{
}

search_server::SearchServer::OperationGuard::OperationGuard(
    const SearchServer& server)
    : server_(const_cast<SearchServer*>(&server)),
      active_(server.beginOperation())
{
}

search_server::SearchServer::OperationGuard::~OperationGuard()
{
    if (active_)
        server_->finishOperation();
}

bool search_server::SearchServer::beginOperation() const
{
    std::lock_guard<std::mutex> lock(operationsMutex_);
    if (stopping_.load(std::memory_order_acquire))
        return false;
    ++activeOperations_;
    return true;
}

void search_server::SearchServer::finishOperation() const noexcept
{
    std::lock_guard<std::mutex> lock(operationsMutex_);
    if (activeOperations_ > 0)
        --activeOperations_;
    if (activeOperations_ == 0)
        operationsCondition_.notify_all();
}

void search_server::SearchServer::requestStop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(operationsMutex_);
        stopping_.store(true, std::memory_order_release);
    }
    must_start_update = false;
    cv_search_server.notify_all();
    if (index)
        index->requestStop();
}

void search_server::SearchServer::wait()
{
    std::unique_lock<std::mutex> lock(operationsMutex_);
    operationsCondition_.wait(lock, [this]() {
        return activeOperations_ == 0;
    });
    lock.unlock();
    if (index)
        index->waitForIdle();
}

void search_server::SearchServer::stop()
{
    if (stopped_.exchange(true, std::memory_order_acq_rel))
        return;
    requestStop();
    wait();
    if (index)
        index->saveIndex();
}


