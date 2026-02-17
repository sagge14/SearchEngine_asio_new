#include <boost/asio.hpp>
#include "MultiWatcher.h"
#include "MyUtils/LogFile.h"
#include <windows.h>
#include <filesystem>

MultiDirWatcher::MultiDirWatcher(const std::wstring& p,
                                 const std::unordered_set<std::wstring>& kids,
                                 FileCb   fcb,
                                 boost::asio::io_context* io)
        : parentDir_(p), kids_(kids), fileCb_(std::move(fcb)), io_(io)
{}

MultiDirWatcher::~MultiDirWatcher() { stop(); }

void MultiDirWatcher::start()
{
    if (running_ || !io_) return;
    running_ = true;

    /* ───── создаём watcher для каждой подпапки из конфига (kids); если каталога нет — start() вернёт false ───── */
    namespace fs = std::filesystem;
    for (const auto& name : kids_)
    {
        if (inner_.contains(name)) continue;

        std::wstring childPath = (fs::path(parentDir_) / name).wstring();
        auto w = std::make_unique<FileWatcher>(*io_, childPath, fileCb_, /*forceWalk=*/false);
        if (!w->start()) continue;   // каталог не существует — подхватит watchParent() при появлении

        inner_.emplace(name, std::move(w));
        LogFile::getWatcher().write(L"[Multi] start init watcher for " + name);
    }

    /* ───── основной поток слежения за переименованиями (отдельный поток, чтобы не блокировать scheduler) ───── */
    if (watchThread_.joinable())
        watchThread_.join();
    watchThread_ = std::thread(&MultiDirWatcher::watchParent, this);
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

    if (watchThread_.joinable())
        watchThread_.join();
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
                LogFile::getWatcher().write(L"[Multi] can't open parent: " + parentDir_);
                std::this_thread::sleep_for(SLEEP);
                continue;
            }
            LogFile::getWatcher().write(L"[Multi] watching parent " + parentDir_);
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

            bool interesting = kids_.contains(name);

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
                            LogFile::getWatcher().write(L"[Multi] start watcher for " + name);
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
                            LogFile::getWatcher().write(L"[Multi] stop watcher for " + name);
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
