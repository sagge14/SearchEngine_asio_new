#pragma once

#include "FileWatcher.h"
#include <unordered_map>

namespace boost::asio {
    class io_context;
}


class MultiDirWatcher
{
public:
    using FileCb   = FileWatcher::Callback;
    using DirMatch = std::function<bool(const std::wstring&)>; // true → слежка

    MultiDirWatcher(const std::wstring& parentDir,
                    DirMatch            matchFn,   // какие подпапки ловим
                    FileCb              fileCb,
                    boost::asio::io_context* io);   // событие по файлам

    ~MultiDirWatcher();
    void start();
    void stop();

private:
    void watchParent();                 // поток-наблюдатель

    std::wstring parentDir_;
    DirMatch     matchFn_;
    FileCb       fileCb_;

    HANDLE              hParent_{INVALID_HANDLE_VALUE};
    std::atomic<bool>   running_{false};
    boost::asio::io_context* io_;

    // name  → активный вотчер
    std::unordered_map<std::wstring,std::unique_ptr<FileWatcher>> inner_;
};
