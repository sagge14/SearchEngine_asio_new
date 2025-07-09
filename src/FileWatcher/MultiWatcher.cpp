#include <boost/asio.hpp>
#include "MultiWatcher.h"
#include <windows.h>
#include <iostream>
#include <filesystem>

MultiDirWatcher::MultiDirWatcher(const std::wstring& p,
                                 DirMatch match,
                                 FileCb   fcb,
                                boost::asio::io_context* io)
        : parentDir_(p), matchFn_(std::move(match)), fileCb_(std::move(fcb)), io_(io)
{}

MultiDirWatcher::~MultiDirWatcher() { stop(); }

void MultiDirWatcher::start()
{
    if (running_ || !io_) return;
    running_ = true;

    /* ───── стартовый скан ───── */
    namespace fs = std::filesystem;
    try {
        for (const auto& e :
                fs::directory_iterator(parentDir_,
                                       fs::directory_options::skip_permission_denied))
        {
            if (!e.is_directory()) continue;

            std::wstring name = e.path().filename().wstring();
            if (!matchFn_(name))  continue;            // неинтересно

            if (!inner_.contains(name))                // ещё нет watcher-а
            {
                auto w = std::make_unique<FileWatcher>(*io_,
                        e.path().wstring(), fileCb_, /*forceWalk=*/false );
                w->start();
                inner_.emplace(name, std::move(w));

                std::wcout << L"[Multi] start init watcher for "
                           << name << std::endl;
            }
        }
    }
    catch (...) {
        std::wcerr << L"[Multi] initial scan failed for "
                   << parentDir_ << std::endl;
    }

   // /* ───── основной поток слежения за переименованиями ───── */
   // thread_ = std::thread(&MultiDirWatcher::watchParent, this);

    auto watch = [this]
    {
        this->watchParent();
    };

    io_->post(watch);
}


void MultiDirWatcher::stop()
{
    if (!running_) return;
    running_ = false;

    if (hParent_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(hParent_, nullptr);
        CloseHandle(hParent_);
        hParent_ = INVALID_HANDLE_VALUE;
    }
    for (auto& [_, w] : inner_) w->stop();
    inner_.clear();

}

void MultiDirWatcher::watchParent()
{
    constexpr auto SLEEP = std::chrono::seconds(4);
    char  buf[1024];
    DWORD bytes{0};

    while (running_)
    {
        /* 1. открываем parent-dir, если ещё нет */
        if (hParent_ == INVALID_HANDLE_VALUE)
        {
            hParent_ = CreateFileW(
                    parentDir_.c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);

            if (hParent_ == INVALID_HANDLE_VALUE) {
                std::wcerr << L"[Multi] can't open parent: " << parentDir_ << std::endl;
                std::this_thread::sleep_for(SLEEP);
                continue;
            }
            std::wcout << L"[Multi] watching parent " << parentDir_ << std::endl;
        }

        /* 2. ждём изменения имён директорий */
        BOOL ok = ReadDirectoryChangesW(
                hParent_, buf, sizeof(buf), FALSE,
                FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytes, nullptr, nullptr);

        if (!ok) {
            CloseHandle(hParent_);
            hParent_ = INVALID_HANDLE_VALUE;
            std::this_thread::sleep_for(SLEEP);
            continue;
        }

        /* 3. разбираем события */
        DWORD off = 0;
        do {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf + off);
            std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));

            bool interesting = matchFn_(name);   // применять ваш фильтр

            if (interesting)
            {
                auto childPath = (std::filesystem::path(parentDir_) / name).wstring();

                switch (fni->Action)
                {
                    case FILE_ACTION_ADDED:
                    case FILE_ACTION_RENAMED_NEW_NAME:
                    {
                        if (!inner_.contains(name))
                        {
                            auto w = std::make_unique<FileWatcher>(*io_,childPath, fileCb_, true /*forceWalk*/ );
                            w->start();
                            inner_.emplace(name, std::move(w));
                            std::wcout << L"[Multi] start watcher for " << name << std::endl;
                        }
                        break;
                    }
                    case FILE_ACTION_REMOVED:
                    case FILE_ACTION_RENAMED_OLD_NAME:
                    {
                        if (auto it = inner_.find(name); it != inner_.end())
                        {
                            it->second->stop();
                            inner_.erase(it);
                            std::wcout << L"[Multi] stop watcher for " << name << std::endl;
                        }
                        break;
                    }
                }
            }

            if (fni->NextEntryOffset == 0) break;
            off += fni->NextEntryOffset;
        } while (off < bytes);
    }
}
