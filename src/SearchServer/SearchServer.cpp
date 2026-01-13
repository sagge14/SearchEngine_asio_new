//

// Created by user on 02.02.2023.
//
#include "SearchServer.h"
#include <iostream>
#include <string>
#include <codecvt>
#include "Interface.h"

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

namespace my_conv  {
    // Преобразуем std::wstring в UTF-8 std::string
    std::string wstringToUtf8(const std::wstring &wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

    // Преобразуем UTF-8 std::string в std::wstring
    std::wstring utf8ToWstring(const std::string &str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(str);
    }
}


search_server::setFileInd search_server::SearchServer::intersectionSetFiles(const set<string> &request) const {


    if(request.empty())
        return {};

    setFileInd result, first;
    list<Word> wordList;

    auto getSetFromMap = [this](const std::string& word) -> setFileInd
    {
        setFileInd s;

            auto post = index->getPostingCopyByWord(word);
        if (post)
        {
            std::cout << "[NEW] " << word << '\n';          // ← ВРЕМЕННЫЙ ЛОГ
            for (const auto& [fileId, _] : *post)
                s.insert(fileId);
            return s;
        }
        return {};

    };



    for (const auto& word : request)
    {

        auto post = index->getPostingCopyByWord(word);

        if (post)
        {
            wordList.emplace_back(word, post->size());
        }
            /* 2.  Если слово не найдено и сервер НЕ в режиме exact-search —
                   просто пропускаем его (как и раньше).                   */
        else if (!settings.exactSearch)
        {
            continue;
        }
            /* 3.  Режим exact-search и слово отсутствует —
                   весь запрос не может быть выполнен.                     */
        else
        {
            return {};          // мгновенно выходим с пустым результатом
        }
    }


    if(!settings.exactSearch)
    {
        for(const auto& w: wordList)
        {
            auto sSet = getSetFromMap(w.word);
            result.insert(sSet.begin(),sSet.end());
        }
        return result;
    }

    wordList.sort();
    result = getSetFromMap(wordList.front().word);

    while(true)
    {
        if(next(wordList.begin()) != wordList.end())
        {
            wordList.pop_front();
            first = getSetFromMap(wordList.front().word);
        }
        else
            return result;

        setFileInd intersection;

        set_intersection(result.begin(),result.end(),
                         first.begin(),first.end(),
                         std::inserter(intersection, intersection.end()));

        if(intersection.empty())
            return {};
        else
            result = intersection;
    }
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

    work = true;

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
        Results.emplace_back(fileInd, request, index, settings.exactSearch);

    Results.sort();

    int i = 0;
    for(const auto& r:Results)
    {
        if(!settings.dir.empty())
        //    out.emplace_back(to_string(r.fileInd),r.getRelativeIndex());
       // else
            out.emplace_back(r.filePath,r.getRelativeIndex());

        i++;
        if(i == settings.maxResponse)
        {

            work = false;
            std::cout << "Request finish!!!" << endl;
            return out;
        }

    }

 //   updateM.unlock();
    std::cout << "Request finish!!!" << endl;
    work = false;

    if(must_start_update)
        cv_search_server.notify_one();


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
    /**
    Формируем лист ответов на все запросы, с возможностью выбора, что использовать в качестве
     идентификатора файла - индекс или текст запроса.*/

    listAnswers out;
    int i = 1;

    for(auto& request: requests)
        if(settings.requestText)
            out.emplace_back(getAnswer(request), request);
        else
        {
            string nRequest = i < 10 ? "00" + to_string(i) : i < 100 ? "0" + to_string(i) : to_string(i);
            out.emplace_back(getAnswer(request), "request" + nRequest);
            i++;
        }

    return out;
}

void search_server::SearchServer::updateDocumentBase(const std::vector<std::wstring> & _docPaths) {
    /**
    Запускаем обновление базы индексов, записываем в @param time сколько времени уйдет на индексацию. */

    time = inverted_index::perf_timer<chrono::milliseconds>::duration([this,_docPaths]() {
        std::future<void> fut = this->index->updateDocumentBase(_docPaths);

        // ЖДЁМ полного завершения (compact, save, batch commits, всё!)
        fut.get();
        }).count();

}


search_server::SearchServer::SearchServer(const Settings& _settings , boost::asio::io_context& _io_context, boost::asio::io_context& _io_commit) : time{},
index(), io_context{_io_context}, io_commit{_io_commit}
{

    settings = (_settings);
    trustSettings();

  //  initWatchers(settings);
    index = new inverted_index::InvertedIndex(io_context, io_commit);
    boost::asio::post(io_context, []{ std::cout << " SearchServer::SearchServer ASYNC TEST!\n"; });
    // Создаем поток для update и оборачиваем его в перезапускающий мониторинг

}

void search_server::SearchServer::trustSettings() const {
    /**
    Функция проверяет корректность настроек сервера:
     1. Имя сервера не может быть пустым.
     2. Количество потоков индексирующих базу не может быть отрицательным.
     3. Файлы для индексирования должны браться либо из папки указанной в @param 'settings.dir'
        либо напрямую из файла настроек сервера (по умолчанию Settings.json).
    Если настройки не корректны выбрасывается соответствующее исключение. */

    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> strconverter;

    if(settings.name.empty())
        throw(myExp(ErrorCodes::NAME));
    if(settings.threadCount < 0)
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

    delete  index;

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
    if (update_is_running || must_start_update)
        return;

    must_start_update = true;

    std::thread([this] {
        try {
            updateStep();
            index->saveIndex();
        } catch (const std::exception& e) {
            // Можно залогировать ошибку
        }
        must_start_update = false; // Если нужен сброс флага — синхронизируй при необходимости!
    }).detach();
}




[[maybe_unused]] bool search_server::SearchServer::getIsUpdateRunning() const {
    std::cout << " get index wordT! - " << index->work << " "  <<true << endl;
    return index->work;
}

search_server::RelativeIndex::RelativeIndex(size_t _fileInd, const set<string>& _request, const inverted_index::InvertedIndex* _index, bool _exactSearch)

{
    /**
    Т.к. сервер может работать в двух режимах: точного поиска и обычного, для которого не обязательно все слова из запроса
     должны одержаться в файле-результате, то во втором случае перед вычисление количества сколько раз слово
     встречается в файле нужно проверить есть ли вообще это слово в словаре @param freqDictionary и есть ли
     это слово в самом документе с индексом @param fileInd - эту проверку осуществляет функция @param checkWordAndFileInd.
     @param sum статическое поле класса, хранит максимальную абсолютную релевантность файла-результата, используется для
     вычисления относительной релевантности.
     */

    using convert_t = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_t, wchar_t> strconverter;

    const std::wstring& wpath = _index->docPaths.pathById(_fileInd);
    filePath = strconverter.to_bytes(wpath);

    auto checkWordAndFileInd = [_index, _fileInd](const std::string& w)
    {
        if (auto post = _index->getPostingCopyByWord(w) ; post)
            return true;

        return false;
    };



    for (const auto& word : _request)
    {
        if (_exactSearch || checkWordAndFileInd(word))
        {
            if (auto post = _index->getPostingCopyByWord(word); post)
            {
                if (const uint16_t* p = post->find(_fileInd))
                    sum += *p;
            }
            // else — если нужен резерв через старый freqDictionary, оставьте закомментированным
            //     sum += _index->freqDictionary.at(word).at(_fileInd);
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
    std::cout << std::endl << "Name:\t\t\t\t" << name << std::endl;
    std::cout << "Version:\t\t\t" << version << std::endl;
    std::cout << "Number of maximum responses:\t" << maxResponse << std::endl;
    if(!dir.empty())
        std::cout << "The directory for indexing:\t" << dir << std::endl;
    std::cout << "Thread count:\t\t\t";
    if(threadCount)
        std::cout << threadCount << std::endl;
    else
        std::cout << std::thread::hardware_concurrency() << std::endl;
    std::cout << "Index database update period:\t" << indTime << " seconds" << std::endl;
    std::cout << "Asio port:\t\t\t" << port << std::endl;
    std::cout << "Show request as text:\t\t" << std::boolalpha << requestText << std::endl;
    std::cout << "Use exact search:\t\t" << std::boolalpha << exactSearch << std::endl << std::endl;
}

search_server::Settings::Settings() {

    name = "TestServer";
    version = "1.1";
    dir = "";
    threadCount = 1;
    maxResponse = 5;
    port = 15001;
    exactSearch = false;

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
        index = new inverted_index::InvertedIndex(io_context, io_commit);
        addToLog("updateStep() → index created");
    }

    /* ---------- СКАНИРОВАНИЕ ---------- */
    addToLog("updateStep() → scan start");

    docPaths = Interface::getAllFilesFromDirs(
            settings.dirs,
            settings.extensions,
            settings.excludeDirs
    );

    addToLog("updateStep() → scan done, files=" +
             std::to_string(docPaths.size()));

    /* ---------- ОЖИДАНИЕ ПОИСКА ---------- */
    addToLog("updateStep() → wait search stop");

    {
        std::unique_lock<std::shared_mutex> lock{searchM};

        bool ok = cv_search_server.wait_for(
                lock,
                std::chrono::seconds(10),
                [this] { return !work; }
        );

        addToLog(std::string("updateStep() → wait done, result=") +
                 (ok ? "OK" : "TIMEOUT") +
                 " work=" + std::to_string(work));
    }

    /* ---------- СТАРТ ОБНОВЛЕНИЯ ---------- */
    update_is_running = true;
    must_start_update = false;

    logState("before updateDocumentBase", update_is_running, work, index->work);


    std::future<void> fut = std::async(
            std::launch::async,
            &search_server::SearchServer::updateDocumentBase,
            this,
            docPaths
    );

    addToLog("updateStep() → updateDocumentBase started");

    /* ---------- ОЖИДАНИЕ ИНДЕКСАЦИИ ---------- */
    addToLog("updateStep() → waiting updateDocumentBase");

    fut.wait();

    addToLog("updateStep() → updateDocumentBase finished");

    /* ---------- ФИНАЛ ---------- */
    index->compact();
    addToLog("updateStep() → compact done");

    index->saveIndex();
    addToLog("updateStep() → saveIndex done");

    time = getTimeOfUpdate();

    addToLog("Index database update completed! " +
             std::to_string(index->docPaths.size()) + " files, " +
             std::to_string(index->dictionary.size()) +
             " unique words. Time " +
             std::to_string(time) + " sec");

    update_is_running = false;
    must_start_update = false;

    cv_search_server.notify_all();

    logState("updateStep() EXIT", update_is_running, work, index->work);
}




void search_server::SearchServer::addFileToIndex(const std::wstring& path) {
    if(!index)
        return;

    index->enqueueFileUpdate(path);
}

void search_server::SearchServer::removeFileFromIndex(const std::wstring &path) {
    if(!index)
        return;
    index->enqueueFileDeletion(path);
}


