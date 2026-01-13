#include "ContextRuntime.h"

ContextRuntime::Context::Context()
        : guard(boost::asio::make_work_guard(io)) {}

void ContextRuntime::Context::stop() {
    guard.reset();
    io.stop();
}

ContextRuntime::ContextRuntime(size_t totalThreads) {
    calcThreads(totalThreads);
}

ContextRuntime::~ContextRuntime() {
    stop();
}

void ContextRuntime::calcThreads(size_t total) {
    size_t base = std::max<size_t>(1, total / 4);
    t_net_       = base;
    t_scheduler_ = base;
    t_index_     = base;
    t_commit_    = std::max<size_t>(1, total - base * 3);
}

void ContextRuntime::start() {
    auto run = [this](Context& ctx, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            threads_.emplace_back([&ctx] {
                ctx.io.run();
            });
        }
    };

    run(net_, t_net_);
    run(scheduler_, t_scheduler_);
    run(index_, t_index_);
    run(commit_, t_commit_);
}

void ContextRuntime::stop() {
    net_.stop();
    scheduler_.stop();
    index_.stop();
    commit_.stop();

    for (auto& t : threads_)
        if (t.joinable())
            t.join();
}

boost::asio::io_context& ContextRuntime::net()       { return net_.io; }
boost::asio::io_context& ContextRuntime::scheduler(){ return scheduler_.io; }
boost::asio::io_context& ContextRuntime::index()    { return index_.io; }
boost::asio::io_context& ContextRuntime::commit()   { return commit_.io; }
