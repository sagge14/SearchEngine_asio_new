#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <atomic>

class AbstractScheduledTask : public std::enable_shared_from_this<AbstractScheduledTask> {
protected:
    boost::asio::steady_timer timer_;
    std::chrono::seconds period_;
    boost::asio::io_context& io_;
    std::atomic<bool> stopped_{false};

    virtual void runTask() = 0;

    void scheduleNext() {
        if (stopped_) return;
        timer_.expires_after(period_);
        timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec && !self->stopped_) {
                self->runTask();
                self->scheduleNext();
            }
        });
    }
public:
    AbstractScheduledTask(boost::asio::io_context& io, std::chrono::seconds period)
            : timer_(io), period_(period), io_(io) {}


    void start() {
        stopped_ = false;
        scheduleNext();
    }
    void stop() {
        stopped_ = true;
        timer_.cancel();
    }
    virtual ~AbstractScheduledTask() = default;
};
