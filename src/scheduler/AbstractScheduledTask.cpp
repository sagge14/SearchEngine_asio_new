#include "AbstractScheduledTask.h"

AbstractScheduledTask::AbstractScheduledTask(
        boost::asio::io_context& scheduler_io,
        boost::asio::any_io_executor cpu_ex,
        std::chrono::seconds period,
        bool run_immediately)
        : timer_(scheduler_io)
        , period_(period)
        , cpu_ex_(std::move(cpu_ex))
        , run_immediately_(run_immediately)
{
}

void AbstractScheduledTask::start()
{
    stopped_.store(false, std::memory_order_relaxed);
    if (run_immediately_)
        dispatchRun();
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

                self->dispatchRun();

                self->scheduleNext();
            }
    );
}

void AbstractScheduledTask::dispatchRun()
{
    boost::asio::post(
            cpu_ex_,
            [self = shared_from_this()]
            {
                if (!self->stopped_.load(std::memory_order_relaxed))
                    self->runTask();
            }
    );
}
