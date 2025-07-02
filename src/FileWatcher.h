#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include <filesystem>

enum class FileEvent {
    Added, Removed, Modified, RenamedOld, RenamedNew
};

class FileWatcher
{
public:
    using Callback = std::function<void(FileEvent, const std::wstring&)>;

    /*  forceWalk = true → при ПЕРВОМ открытии каталога     *
     *  сразу отправить Added для всех файлов внутри.       */
    FileWatcher(const std::wstring& directory,
                Callback            cb,
                bool                forceWalk = false);

    ~FileWatcher();

    void start();
    void stop();

private:
    /* helpers */
    bool openDir();                // попытка открыть каталог
    void closeDir();               // закрыть hDir_
    void fireAddEventsRecursive(); // Added для всех файлов

    void watch();                  // главный цикл

    /* data */
    std::wstring    directory_;
    Callback        callback_;
    bool            forceWalk_;          // требовать mass-Added
    bool            initialDirPresent_;  // был ли при старте
    bool            everOpened_{false};  // хотя бы раз открыт

    std::atomic<bool> running_{false};
    HANDLE          hDir_{INVALID_HANDLE_VALUE};
    std::thread     watchThread_;
};
