#include "FileWatcherManager.h"
#include <boost/asio/post.hpp>

FileWatcherManager::FileWatcherManager(boost::asio::thread_pool& pool)
        : pool_(pool)
{}

FileWatcherManager::~FileWatcherManager() {
    stop();
}

void FileWatcherManager::addWatch(const std::wstring& directory) {
    directories_.push_back(directory);
}

void FileWatcherManager::start(EventHandler handler) {
    handler_ = handler;
    for (const auto& dir : directories_) {
        watchers_.emplace_back(
                std::make_unique<FileWatcher>(
                        dir,
                        [this, dir](FileEvent evt, const std::wstring& fname) {
                            // Событие уходит в boost-пул
                            boost::asio::post(pool_, [this, evt, dir, fname] {
                                if (handler_) handler_(evt, dir, fname);
                            });
                        }, io_
                )
        );
    }
    for (auto& w : watchers_) w->start();
}

void FileWatcherManager::stop() {
    for (auto& w : watchers_) w->stop();
    watchers_.clear();
}
