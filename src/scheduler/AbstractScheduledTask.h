#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>

class AbstractScheduledTask
        : public std::enable_shared_from_this<AbstractScheduledTask>
{
protected:
    boost::asio::steady_timer timer_;
    std::chrono::seconds period_;
    std::atomic<bool> stopped_{false};
    boost::asio::any_io_executor cpu_ex_;
    bool run_immediately_;
    std::mutex run_mutex_;
    std::condition_variable run_condition_;
    std::size_t active_runs_{0};

    virtual void runTask() = 0;
    void dispatchRun();
    void scheduleNext();
    void finishRun() noexcept;

public:
    AbstractScheduledTask(boost::asio::io_context& scheduler_io,
                          boost::asio::any_io_executor cpu_ex,
                          std::chrono::seconds period,
                          bool run_immediately = false);

    void start();
    void stop();
    void wait();

    virtual ~AbstractScheduledTask() = default;
};
