// PeriodicUpdateTask.h
#pragma once
#include "scheduler/AbstractScheduledTask.h"
#include <memory>
#include "SearchServer.h"

class PeriodicUpdateTask : public AbstractScheduledTask {
    search_server::SearchServer *server_;
public:
    PeriodicUpdateTask(boost::asio::io_context& io, std::chrono::seconds period, search_server::SearchServer* server)
            : AbstractScheduledTask(io, period), server_(server) {}

    void runTask() override {
        // просто вызываем публичную функцию у сервера
        if (server_) {
            server_->updateStep();
        }
    }
};
