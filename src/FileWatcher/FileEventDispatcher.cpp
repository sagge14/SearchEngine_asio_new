//
// Created by Sg on 06.07.2025.
//

#include "FileEventDispatcher.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include <unordered_set>

static const wchar_t* evtToStr(FileEvent e)
{
    switch (e) {
        case FileEvent::Added:       return L"Added";
        case FileEvent::Removed:     return L"Removed";
        case FileEvent::Modified:    return L"Modified";
        case FileEvent::RenamedOld:  return L"RenamedOld";
        case FileEvent::RenamedNew:  return L"RenamedNew";
    }
    return L"?";
}

FileEvent merge2(FileEvent old, FileEvent neu)
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

void FileEventDispatcher::pushFileEvent(FileEvent evt, const std::wstring& path)
{
    if (!matchByExtensions(path)) {
        LogFile::getWatcher().write(L"[Dispatcher] pushFileEvent SKIP (ext) path=" + path);
        return;
    }
    LogFile::getWatcher().write(L"[Dispatcher] pushFileEvent " + std::wstring(evtToStr(evt)) + L" path=" + path);

    size_t h = std::hash<std::wstring>{}(path);

    std::lock_guard lk(mtx_);

    auto& st = evtMap_[h];          // создаётся по необходимости
    st.evt  = (st.queued) ? merge2(st.evt, evt) : evt;
    st.path = path;

    if (!st.queued) {               // впервые — кладём в очередь
        pendingQ_.push(h);
        st.queued = true;
    }
}

void FileEventDispatcher::flushPending()
{
    std::queue<size_t> local;

    {   std::lock_guard lk(mtx_);
        std::swap(local, pendingQ_);
    }

    const size_t qsize = local.size();
    if (qsize > 0)
        LogFile::getWatcher().write(std::string("[Dispatcher] flushPending start queue=") + std::to_string(qsize));

    while (!local.empty())
    {
        size_t h = local.front(); local.pop();
        FileEvent evt; std::wstring path;

        {   std::lock_guard lk(mtx_);
            auto it = evtMap_.find(h);
            if (it == evtMap_.end()) continue;      // уже стерли
            evt  = it->second.evt;
            path = it->second.path;
            evtMap_.erase(it);                      //   ← снимаем блок
        }

        LogFile::getWatcher().write(L"[Dispatcher] flushPending dispatch " + std::wstring(evtToStr(evt)) + L" path=" + path);

        switch (evt)
        {
            case FileEvent::Removed:
            case FileEvent::RenamedOld:
                if(commands_.find(FileEvent::Removed) != commands_.end())
                    commands_.at(FileEvent::Removed)->execute(path);
                break;
            default:                                // Added / Modified / Ren.New
                if(commands_.find(FileEvent::Added) != commands_.end())
                    commands_.at(FileEvent::Added)->execute(path);
                break;
        }
    }
}

void FileEventDispatcher::initWatchers(const std::vector<std::string>& _dirs)
{
    /* parent-dir → множество имён нужных подпапок */
    std::unordered_map<std::wstring,
            std::unordered_set<std::wstring>> need;

    for (const auto& d8 : _dirs)
    {
        std::filesystem::path p = encoding::utf8_to_wstring(d8);
        std::wstring parent = p.parent_path().wstring();   // напр.  L"D:\\"
        std::wstring name   = p.filename().wstring();      // напр.  L"Data"
        LogFile::getWatcher().write(L"[enqueueUpdate] " + name + L" " + parent);
        need[parent].insert(name);
    }

    /* callback для файлов (из SearchServer) */
    auto fileCb = [this](FileEvent evt, const std::wstring& full)
    {
        pushFileEvent(evt, full);
    };

    /* создаём по parent-каталогу один MultiDirWatcher; передаём набор kids из конфига */
    for (auto& [parent, kids] : need)
    {
        /* фильтр: интересны только каталоги, имя которых в “kids” */
        auto w = std::make_unique<MultiDirWatcher>(parent, kids, fileCb, &io_);
        w->start();
        dirWatchers_.emplace_back(std::move(w));
    }
}

FileEventDispatcher::FileEventDispatcher(const std::vector<std::string>& _dirs,
                                         const std::vector<std::string>& _ext,
                                         boost::asio::io_context& io) : io_(io), dirs_(_dirs), ext_(_ext)
{

    initWatchers(dirs_);

}

bool FileEventDispatcher::matchByExtensions(const std::wstring& path) const
{
    using namespace std::filesystem;

    /* Если список пуст -- фильтра нет → разрешаем всё */
    if (ext_.empty())
        return true;

    std::wstring file = std::filesystem::path(path).filename().wstring();

    // позиция последней точки
    auto pos = file.rfind(L'.');                 // npos ⇒ нет точки
    bool hasDot   = pos != std::wstring::npos;
    bool dotAtEnd = hasDot && pos == file.size() - 1;
    bool noExt    = !hasDot || dotAtEnd;         // «без расширения»

    for (const auto& e8 : ext_)
    {
        std::wstring e (e8.begin(), e8.end());   // UTF-8 → UTF-16

        /*  Пустая строка в конфиге значит: «разрешить файлы без расширения» */
        if (e.empty())
        {
            if (noExt)
                return true;
            continue;
        }

        /*  Файл имеет расширение → сравниваем нечувствительно к регистру  */
        if (!noExt && e.size() <= file.size() - pos - 1 &&
            std::equal(e.begin(), e.end(),
                       file.end() - e.size(),
                       [](wchar_t a, wchar_t b){
                           return towlower(a) == towlower(b);
                       }))
            return true;
    }
    return false;
}

void FileEventDispatcher::stopAll() {
    for(auto& w:dirWatchers_)
        w->stop();
}
