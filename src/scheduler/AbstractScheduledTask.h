#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <atomic>

class AbstractScheduledTask
        : public std::enable_shared_from_this<AbstractScheduledTask>
{
protected:
    boost::asio::steady_timer timer_;
    std::chrono::seconds period_;
    std::atomic<bool> stopped_{false};
    boost::asio::any_io_executor cpu_ex_;

    virtual void runTask() = 0;
    void scheduleNext();

public:
    AbstractScheduledTask(boost::asio::io_context& scheduler_io,
                          boost::asio::any_io_executor cpu_ex,
                          std::chrono::seconds period);

    void start();
    void stop();

    virtual ~AbstractScheduledTask() = default;
};
