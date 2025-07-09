#include "FileWatcher.h"
#include <boost/system/error_code.hpp>
#include <iostream>

/* ───────── ctor / dtor ───────── */

FileWatcher::FileWatcher(boost::asio::io_context& io,
                         const std::wstring&      dir,
                         Callback                 cb,
                         bool                     forceWalk)
        : io_(io)
        , dir_(dir)
        , cb_(std::move(cb))
        , forceWalk_(forceWalk)
{
    buf_.resize(16 * 1024);
    std::error_code ec;
    initialDirPresent_ = std::filesystem::exists(dir_, ec);
}

FileWatcher::~FileWatcher()
{
    stop();
}

/* ───────── public ───────── */

bool FileWatcher::start()
{
    if (running_) return true;
    running_ = true;
    return openDir();             // openDir сразу запускает first read
}

void FileWatcher::stop()
{
    if (!running_) return;
    running_ = false;

    if (hDir_ != INVALID_HANDLE_VALUE) {
        ::CancelIoEx(hDir_, nullptr);
        ::CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
    }
}

/* ───────── helpers ───────── */

bool FileWatcher::openDir()
{
    hDir_ = ::CreateFileW(
            dir_.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

    if (hDir_ == INVALID_HANDLE_VALUE) {
        std::wcerr << L"[Watcher] can't open " << dir_ << L'\n';
        return false;
    }

    boost::system::error_code ec;
    auto& iocp =
            boost::asio::use_service<boost::asio::detail::win_iocp_io_context>(io_);
    iocp.register_handle(hDir_, ec);
    if (ec) {
        std::wcerr << L"[Watcher] register_handle error: "
                   << ec.message().c_str() << L'\n';
        ::CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
        return false;
    }

    /* ───── решаем, нужен ли mass-Added ───── */
    bool needWalk = forceWalk_
                    || (!everOpened_ && !initialDirPresent_)  // «появился впервые»
                    || ( everOpened_);                       // повторное открытие
    everOpened_ = true;
    if (needWalk) fireAddEventsRecursive();

    startRead();          // первая асинхронная операция
    return true;
}

void FileWatcher::startRead()
{
    if (!running_ || hDir_ == INVALID_HANDLE_VALUE) return;

    // 1. создаём shared_ptr без handler-а
    auto op = std::make_shared<boost::asio::windows::overlapped_ptr>();

    // 2. задаём completion-handler после создания
    op->reset(                                    // ← 2 аргумента
            io_,                                      // execution_context
            [this, op](const boost::system::error_code& ec,
                       std::size_t                    bytes)
            {
                if (!ec && bytes) {
                    parseEvents(bytes);
                    startRead();                      // перезапуск
                }
                else if (ec != boost::asio::error::operation_aborted)
                    std::wcerr << L"[Watcher] IOCP error: "
                               << ec.message().c_str() << L'\n';
            });

    DWORD dummy = 0;
    BOOL ok = ReadDirectoryChangesW(
            hDir_, buf_.data(), static_cast<DWORD>(buf_.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME  |
            FILE_NOTIFY_CHANGE_LAST_WRITE,
            &dummy,
            op->get(), nullptr);

    if (!ok && GetLastError() != ERROR_IO_PENDING)
    {
        boost::system::error_code ec(GetLastError(),
                                     boost::system::system_category());
        op->complete(ec, 0);             // немедленно вызываем handler с ошибкой
    }
    else
    {
        op->release();                   // передаём владение Asio
    }
}

static FileEvent toEvt(DWORD act)
{
    switch (act)
    {
        case FILE_ACTION_ADDED:            return FileEvent::Added;
        case FILE_ACTION_REMOVED:          return FileEvent::Removed;
        case FILE_ACTION_MODIFIED:         return FileEvent::Modified;
        case FILE_ACTION_RENAMED_OLD_NAME: return FileEvent::RenamedOld;
        case FILE_ACTION_RENAMED_NEW_NAME: return FileEvent::RenamedNew;
    }
    return FileEvent::Modified;
}

void FileWatcher::parseEvents(std::size_t bytes)
{
    BYTE* ptr = reinterpret_cast<BYTE*>(buf_.data());
    size_t off = 0;
    while (off < bytes)
    {
        auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr + off);
        std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
        std::filesystem::path abs = std::filesystem::path(dir_) / rel;

        if (cb_) cb_(toEvt(fni->Action), abs.wstring());

        if (!fni->NextEntryOffset) break;
        off += fni->NextEntryOffset;
    }
}

void FileWatcher::fireAddEventsRecursive()
{
    namespace fs = std::filesystem;
    try {
        for (auto& e : fs::recursive_directory_iterator(
                dir_, fs::directory_options::skip_permission_denied))
        {
            if (e.is_regular_file() && cb_)
                cb_(FileEvent::Added, e.path().wstring());
        }
    }
    catch (...) {
        /* каталог может исчезнуть — игнорируем */
    }
}
