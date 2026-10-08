#include "scheduler/AbstractScheduledTask.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

namespace {

class CountingScheduledTask final : public AbstractScheduledTask {
public:
    CountingScheduledTask(boost::asio::io_context& scheduler,
                          boost::asio::any_io_executor executor,
                          bool run_immediately,
                          std::atomic<int>& run_count)
        : AbstractScheduledTask(
              scheduler,
              std::move(executor),
              std::chrono::hours(1),
              run_immediately
          ),
          run_count_(run_count)
    {
    }

private:
    void runTask() override
    {
        run_count_.fetch_add(1);
    }

    std::atomic<int>& run_count_;
};

TEST(AbstractScheduledTask, ImmediateModeDispatchesBeforeFirstPeriod)
{
    boost::asio::io_context scheduler;
    boost::asio::thread_pool cpu_pool(1);
    std::atomic<int> run_count{0};
    auto task = std::make_shared<CountingScheduledTask>(
        scheduler,
        cpu_pool.get_executor(),
        true,
        run_count
    );

    task->start();
    cpu_pool.join();
    task->stop();

    EXPECT_EQ(1, run_count.load());
}

TEST(AbstractScheduledTask, DefaultModeWaitsForFirstPeriod)
{
    boost::asio::io_context scheduler;
    boost::asio::thread_pool cpu_pool(1);
    std::atomic<int> run_count{0};
    auto task = std::make_shared<CountingScheduledTask>(
        scheduler,
        cpu_pool.get_executor(),
        false,
        run_count
    );

    task->start();
    cpu_pool.join();
    task->stop();

    EXPECT_EQ(0, run_count.load());
}

} // namespace
