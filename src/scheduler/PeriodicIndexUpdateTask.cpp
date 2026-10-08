#include "PeriodicIndexUpdateTask.h"

#include <utility>
#include "SearchServer/SearchServer.h"

PeriodicIndexUpdateTask::PeriodicIndexUpdateTask(boost::asio::io_context& io,
                                                 boost::asio::any_io_executor cpu_ex,
                                                 std::chrono::seconds period,
                                                 search_server::SearchServer* server,
                                                 bool run_immediately)
        : AbstractScheduledTask(io, std::move(cpu_ex), period, run_immediately)
        , server_(server)
{
}

void PeriodicIndexUpdateTask::runTask() {
    if (server_) {
        server_->updateStep();
    }
}
