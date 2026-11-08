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
    stopped_.store(false, std::memory_order_release);
    if (run_immediately_)
        dispatchRun();
    scheduleNext();
}

void AbstractScheduledTask::stop()
{
    stopped_.store(true, std::memory_order_release);
    boost::system::error_code ec;
    timer_.cancel(ec);
}

void AbstractScheduledTask::scheduleNext()
{
    if (stopped_.load(std::memory_order_acquire))
        return;

    timer_.expires_after(period_);
    timer_.async_wait(
            [self = shared_from_this()](const boost::system::error_code& ec)
            {
                if (ec || self->stopped_.load(std::memory_order_acquire))
                    return;

                self->dispatchRun();

                self->scheduleNext();
            }
    );
}

void AbstractScheduledTask::dispatchRun()
{
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        if (stopped_.load(std::memory_order_acquire))
            return;
        ++active_runs_;
    }

    try {
        boost::asio::post(
                cpu_ex_,
                [self = shared_from_this()]
                {
                    try {
                        if (!self->stopped_.load(std::memory_order_acquire))
                            self->runTask();
                    } catch (...) {
                        // Periodic task failures must not terminate a pool thread.
                    }
                    self->finishRun();
                }
        );
    } catch (...) {
        finishRun();
        throw;
    }
}

void AbstractScheduledTask::finishRun() noexcept
{
    std::lock_guard<std::mutex> lock(run_mutex_);
    if (active_runs_ > 0)
        --active_runs_;
    if (active_runs_ == 0)
        run_condition_.notify_all();
}

void AbstractScheduledTask::wait()
{
    std::unique_lock<std::mutex> lock(run_mutex_);
    run_condition_.wait(lock, [this]() { return active_runs_ == 0; });
}
