#include "ContextRuntime.h"
#include "Interface.h"
#define LG(...) StartLog::instance().write(__VA_ARGS__)

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
    size_t used = base * 3;
    t_commit_ = (total > used) ? (total - used) : 1;
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
