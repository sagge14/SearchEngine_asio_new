#include "AbstractScheduledTask.h"

AbstractScheduledTask::AbstractScheduledTask(
        boost::asio::io_context& scheduler_io,
        boost::asio::any_io_executor cpu_ex,
        std::chrono::seconds period)
        : timer_(scheduler_io)
        , period_(period)
        , cpu_ex_(std::move(cpu_ex))
{
}

void AbstractScheduledTask::start()
{
    stopped_.store(false, std::memory_order_relaxed);
    scheduleNext();
}

void AbstractScheduledTask::stop()
{
    stopped_.store(true, std::memory_order_relaxed);
    boost::system::error_code ec;
    timer_.cancel(ec);
}

void AbstractScheduledTask::scheduleNext()
{
    if (stopped_.load(std::memory_order_relaxed))
        return;

    timer_.expires_after(period_);
    timer_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& ec)
            {
                if (ec || self->stopped_.load(std::memory_order_relaxed))
                    return;

                boost::asio::post(
                        self->cpu_ex_,
                        [self]
                        {
                            if (!self->stopped_.load(std::memory_order_relaxed))
                                self->runTask();
                        }
                );

                self->scheduleNext();
            }
    );
}
