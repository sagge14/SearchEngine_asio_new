#include "FileWatcher.h"
#include <iostream>

/* ───────── ctor / dtor ───────── */
FileWatcher::FileWatcher(const std::wstring& dir,
                         Callback cb,
                         bool     forceWalk)
        : directory_(dir),
          callback_(std::move(cb)),
          forceWalk_(forceWalk)
{
    std::error_code ec;
    initialDirPresent_ = std::filesystem::exists(directory_, ec);
}
FileWatcher::~FileWatcher() { stop(); }

/* ───────── start / stop ───────── */
void FileWatcher::start()
{
    if (running_) return;
    running_ = true;
    watchThread_ = std::thread(&FileWatcher::watch, this);
}
void FileWatcher::stop()
{
    if (!running_) return;
    running_ = false;
    closeDir();
    if (watchThread_.joinable()) watchThread_.join();
}

/* ───────── helpers ───────── */
void FileWatcher::closeDir()
{
    if (hDir_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(hDir_, nullptr);
        CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
    }
}

bool FileWatcher::openDir()
{
    hDir_ = CreateFileW(
            directory_.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (hDir_ == INVALID_HANDLE_VALUE)
        return false;

    std::wcout << L"[Watcher] open: " << directory_ << std::endl;

    /* ─── mass-Added? ─── */
    bool needWalk = forceWalk_
                    || (!everOpened_ && !initialDirPresent_)  // «появился впервые»
                    || ( everOpened_);                        // повторное появление

    everOpened_ = true;
    if (needWalk) fireAddEventsRecursive();

    return true;
}

void FileWatcher::fireAddEventsRecursive()
{
    namespace fs = std::filesystem;
    try {
        for (const auto& e :
                fs::recursive_directory_iterator(directory_,
                                                 fs::directory_options::skip_permission_denied))
            if (e.is_regular_file() && callback_)
                callback_(FileEvent::Added, e.path().wstring());
    } catch (...) { /* каталог мог исчезнуть – игнор */ }
}

/* ───────── основной цикл ───────── */
void FileWatcher::watch()
{
    constexpr auto POLL  = std::chrono::seconds(5);
    constexpr auto RETRY = std::chrono::seconds(1);

    char  buf[4096];
    DWORD bytes{0};

    while (running_)
    {
        /* 1. каталог существует? */
        std::error_code ec;
        bool dirExists = std::filesystem::exists(directory_, ec);

        if (!dirExists)
        {
            if (hDir_ != INVALID_HANDLE_VALUE) {
                std::wcerr << L"[Watcher] disappeared\n";
                closeDir();
            }
            std::this_thread::sleep_for(POLL);
            continue;            // ждать появления
        }

        /* 2. handle ещё не открыт */
        if (hDir_ == INVALID_HANDLE_VALUE)
        {
            if (!openDir()) { std::this_thread::sleep_for(RETRY); continue; }
        }

        /* 3. ждём события */
        BOOL ok = ReadDirectoryChangesW(
                hDir_, buf, sizeof(buf), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME   |
                FILE_NOTIFY_CHANGE_DIR_NAME    |
                FILE_NOTIFY_CHANGE_SIZE        |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytes, nullptr, nullptr);

        if (!ok) {                       // handle «сломался»
            std::wcerr << L"[Watcher] lost handle\n";
            closeDir();
            std::this_thread::sleep_for(RETRY);
            continue;
        }

        /* 4. разбор буфера */
        DWORD off = 0;
        do {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf + off);
            std::wstring rel(fni->FileName, fni->FileNameLength/sizeof(WCHAR));

            FileEvent evt;
            switch (fni->Action) {
                case FILE_ACTION_ADDED:            evt = FileEvent::Added;       break;
                case FILE_ACTION_REMOVED:          evt = FileEvent::Removed;     break;
                case FILE_ACTION_MODIFIED:         evt = FileEvent::Modified;    break;
                case FILE_ACTION_RENAMED_OLD_NAME: evt = FileEvent::RenamedOld;  break;
                case FILE_ACTION_RENAMED_NEW_NAME: evt = FileEvent::RenamedNew;  break;
                default:                           evt = FileEvent::Modified;    break;
            }

            if (callback_)
            {
                std::filesystem::path p = std::filesystem::path(directory_) / rel;
                callback_(evt, p.wstring());           // ПОЛНЫЙ путь
            }

            if (fni->NextEntryOffset == 0) break;
            off += fni->NextEntryOffset;
        } while (off < bytes);
    }
}
