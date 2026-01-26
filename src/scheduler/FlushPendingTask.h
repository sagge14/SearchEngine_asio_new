// PeriodicIndexUpdateTask.h
#pragma once
#include "scheduler/AbstractScheduledTask.h"
#include <memory>
#include <functional>
#include <utility>
#include "SearchServer/SearchServer.h"
#include "FileWatcher/FileEventDispatcher.h"


class FlushPendingTask2 : public AbstractScheduledTask {

    FileEventDispatcher& fileEventDispatcher;

public:

    FlushPendingTask2(boost::asio::io_context& io, boost::asio::any_io_executor cpu_ex,
                      std::chrono::seconds period, FileEventDispatcher& fed)
            : AbstractScheduledTask(io, std::move(cpu_ex), period), fileEventDispatcher(fed) {}

    void runTask() override {
        // просто вызываем публичную функцию у сервера
        fileEventDispatcher.flushPending();
    }

};
