#pragma once

#include "FileWatcher.h"
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace boost::asio {
    class io_context;
}


class MultiDirWatcher
{
public:
    using FileCb   = FileWatcher::Callback;

    MultiDirWatcher(const std::wstring& parentDir,
                    const std::unordered_set<std::wstring>& kids,  // имена подпапок из конфига
                    FileCb              fileCb,
                    boost::asio::io_context* io);   // событие по файлам

    ~MultiDirWatcher();
    void start();
    void stop();

private:
    void watchParent();                 // поток-наблюдатель

    std::wstring parentDir_;
    std::unordered_set<std::wstring> kids_;
    FileCb       fileCb_;

    HANDLE              hParent_{INVALID_HANDLE_VALUE};
    std::atomic<bool>   running_{false};
    boost::asio::io_context* io_;

    // name  → активный вотчер
    std::unordered_map<std::wstring,std::unique_ptr<FileWatcher>> inner_;
    std::thread watchThread_;
};
