#pragma once
#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <map>
#include <string>




class ContextRuntime {
public:
    struct Context {
        boost::asio::io_context io;
        boost::asio::executor_work_guard<
                boost::asio::io_context::executor_type
        > guard;

        Context();
        void stop();
    };

    explicit ContextRuntime(size_t totalThreads);
    ~ContextRuntime();

    boost::asio::io_context& net();
    boost::asio::io_context& scheduler();
    boost::asio::io_context& commit();
    boost::asio::thread_pool& cpu_pool();


    void start();
    void stop();

private:
    void calcThreads([[maybe_unused]] size_t totalThreads);

    Context net_;
    Context scheduler_;
    Context commit_;

    size_t t_net_{1};
    size_t t_scheduler_{1};
    size_t t_commit_{1};

    std::vector<std::thread> threads_;
    // Размер задаётся из ctor: 0 = по числу ядер (авто), иначе из конфига (thread_count).
    // Resize пула в рантайме Boost.Asio не поддерживает.
    boost::asio::thread_pool cpu_pool_;
};
