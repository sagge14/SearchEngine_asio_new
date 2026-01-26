#pragma once

#include "scheduler/AbstractScheduledTask.h"
#include <chrono>

namespace search_server {
    class SearchServer;
}

class PeriodicIndexUpdateTask : public AbstractScheduledTask {
public:
    PeriodicIndexUpdateTask(boost::asio::io_context& io,
                            boost::asio::any_io_executor cpu_ex,
                            std::chrono::seconds period,
                            search_server::SearchServer* server);

protected:
    void runTask() override;

private:
    search_server::SearchServer* server_;
};
