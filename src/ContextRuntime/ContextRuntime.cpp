#include "ContextRuntime.h"
#include "MyUtils/StartLog.h"
#define LG(...) StartLog::instance().write(__VA_ARGS__)

ContextRuntime::Context::Context()
        : guard(boost::asio::make_work_guard(io)) {}

void ContextRuntime::Context::stop() {
    guard.reset();
    io.stop();
}

ContextRuntime::ContextRuntime(size_t totalThreads)
    : cpu_pool_(totalThreads > 0
                ? std::max(size_t(1), static_cast<size_t>(totalThreads))
                : std::max(size_t(1), std::thread::hardware_concurrency()))
{
    calcThreads(totalThreads);
}

ContextRuntime::~ContextRuntime() {
    stop();
}

void ContextRuntime::calcThreads([[maybe_unused]] size_t totalThreads) {
    // Распределение потоков I/O: фиксировано. totalThreads из ctor используется только для размера cpu_pool.
    // net — приём + все сессии (read/write), 2 потока обычно хватает на много соединений.
    // scheduler — таймеры, отложенные задачи; commit — коммит индекса. По 1 достаточно.
    t_net_       = 2;
    t_scheduler_ = 1;
    t_commit_    = 1;
}

void ContextRuntime::start() {
    auto run = [this](Context& ctx, size_t count) {
        for (size_t i = 0; i < count; ++i) {

            try {
                threads_.emplace_back([&ctx] {
                    ctx.io.run();
                });
            } catch (const std::system_error& e) {
                LG("THREAD CREATE FAILED: ", e.what());
            }


        }
    };

    run(net_, t_net_);
    run(scheduler_, t_scheduler_);
    run(commit_, t_commit_);
}

void ContextRuntime::stop() {
    net_.stop();
    scheduler_.stop();
    commit_.stop();

    for (auto& t : threads_)
        if (t.joinable())
            t.join();
}

boost::asio::io_context& ContextRuntime::net()       { return net_.io; }
boost::asio::io_context& ContextRuntime::scheduler() { return scheduler_.io; }
boost::asio::io_context& ContextRuntime::commit()    { return commit_.io; }
boost::asio::thread_pool &ContextRuntime::cpu_pool() { return cpu_pool_; }
