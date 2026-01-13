#pragma once


#include <vector>
#include <string>
#include <boost/asio/thread_pool.hpp>
#include <memory>
#include "FileWatcher/FileWatcher.h"      // Предыдущий FileWatcher (с enum FileEvent и колбэком)

class FileWatcherManager {
public:
    using EventHandler = std::function<void(FileEvent, const std::wstring&, const std::wstring&)>;
    // (event, папка, имя файла)

    FileWatcherManager(boost::asio::thread_pool& pool);
    ~FileWatcherManager();

    // Добавить папку для слежения
    void addWatch(const std::wstring& directory);

    // Запустить все вотчеры с общим колбэком
    void start(EventHandler handler);

    // Остановить все вотчеры
    void stop();

private:
    std::vector<std::unique_ptr<FileWatcher>> watchers_;
    boost::asio::thread_pool& pool_;
    std::vector<std::wstring> directories_;
    EventHandler handler_;
};
