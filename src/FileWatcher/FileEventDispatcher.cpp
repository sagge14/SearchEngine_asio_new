//
// Created by Sg on 06.07.2025.
//

#include "FileEventDispatcher.h"
#include "FileEventFilter.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include <unordered_set>
#include <utility>

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
    if (stopping_.load(std::memory_order_acquire))
        return;
    if (!file_event_filter::shouldAcceptFileEvent(
            evt, path, fileTypes_, excludedSubtrees_))
    {
        if (!file_event_filter::matchesConfiguredExtension(path, fileTypes_)) {
            LogFile::getWatcher().write(
                L"[Dispatcher] pushFileEvent SKIP (ext) path=" + path);
        } else {
            LogFile::getWatcher().write(
                L"[Dispatcher] pushFileEvent SKIP (excluded) path=" + path);
        }
        return;
    }
    LogFile::getWatcher().write(L"[Dispatcher] pushFileEvent " + std::wstring(evtToStr(evt)) + L" path=" + path);

    size_t h = std::hash<std::wstring>{}(path);

    std::lock_guard lk(mtx_);

    if (stopping_.load(std::memory_order_acquire))
        return;

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
    if (stopping_.load(std::memory_order_acquire))
        return;
    std::queue<size_t> local;

    {   std::lock_guard lk(mtx_);
        std::swap(local, pendingQ_);
    }

    const size_t qsize = local.size();
    if (qsize > 0)
        LogFile::getWatcher().write(std::string("[Dispatcher] flushPending start queue=") + std::to_string(qsize));

    while (!local.empty())
    {
        if (stopping_.load(std::memory_order_acquire))
            break;
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

void FileEventDispatcher::initWatchers(const std::vector<std::string>& _indexRoots)
{
    /* parent-dir → множество имён нужных подпапок */
    std::unordered_map<std::wstring,
            std::unordered_set<std::wstring>> need;

    for (const auto& d8 : _indexRoots)
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

FileEventDispatcher::FileEventDispatcher(const std::vector<std::string>& indexRoots,
                                         file_extension_contract::Selection fileTypes,
                                         const std::vector<std::string>& excludedSubtrees,
                                         boost::asio::io_context& io)
        : io_(io)
        , indexRoots_(indexRoots)
        , fileTypes_(std::move(fileTypes))
        , excludedSubtrees_(excludedSubtrees)
{
    initWatchers(indexRoots_);
}

void FileEventDispatcher::stopAll() {
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;
    for(auto& w:dirWatchers_)
        w->stop();

    std::lock_guard lk(mtx_);
    evtMap_.clear();
    std::queue<size_t> empty;
    pendingQ_.swap(empty);
}
