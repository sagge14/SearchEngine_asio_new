#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/windows/overlapped_ptr.hpp>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <Windows.h>

enum class FileEvent { Added, Removed, Modified, RenamedOld, RenamedNew };

class FileWatcher
{
public:
    using Callback = std::function<void(FileEvent, const std::wstring&)>;

    FileWatcher(boost::asio::io_context& io,
                const std::wstring&      dir,
                Callback                 cb,
                bool                     forceWalk = false);

    /** Запустить асинхронное слежение.  true, если handle открыт. */
    bool start();
    /** Остановить наблюдение.  Safe при многократных вызовах. */
    void stop();
    ~FileWatcher();

private:
    /* helpers */
    bool  openDir();                   // открыть каталог (OVERLAPPED)
    void  startRead();                 // инициировать ReadDirectoryChangesW
    void  parseEvents(std::size_t);    // разобрать буфер
    void  fireAddEventsRecursive();    // массовые Added

    boost::asio::io_context& io_;
    std::wstring             dir_;
    Callback                 cb_;

    std::vector<wchar_t> buf_;
    HANDLE               hDir_{INVALID_HANDLE_VALUE};

    bool   forceWalk_{false};
    bool   initialDirPresent_{false};
    bool   everOpened_{false};
    std::atomic<bool> running_{false};
};
