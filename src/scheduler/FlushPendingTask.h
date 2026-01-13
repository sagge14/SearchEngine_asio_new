// PeriodicUpdateTask.h
#pragma once
#include "scheduler/AbstractScheduledTask.h"
#include <memory>
#include <functional>
#include "SearchServer/SearchServer.h"
#include "FileWatcher/FileEventDispatcher.h"


class FlushPendingTask2 : public AbstractScheduledTask {
    std::function<void()> flush;
    FileEventDispatcher& fileEventDispatcher;
public:
    FlushPendingTask2(boost::asio::io_context& io, std::chrono::seconds period, FileEventDispatcher& fed)
            : AbstractScheduledTask(io, period), fileEventDispatcher(fed) {}

    void runTask() override {
        // просто вызываем публичную функцию у сервера
        fileEventDispatcher.flushPending();
    }
};
