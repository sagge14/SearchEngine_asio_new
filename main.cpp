#include <iostream>
#include <thread>
#include <vector>
#include "SearchServer.h"
#include "ConverterJSON.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "AsioServer.h"
#include "Commands/GetTelegaWay/TelegaWay.h"
#include "SqlLogger.h"
#include <exception>
#include <fstream>
#include <string>
#include "scheduler/PeriodicTaskManager.h"
#include "scheduler/TaskID.h"
#include "scheduler/UpdateIndexTask.h"
#include "scheduler/PeriodicUpdateTask.h"
#include "scheduler/FlushPendingTask.h"
#include "scheduler/BackupTask.h"
#include <windows.h>

void myTerminateHandler() {
    std::ofstream log("fatal_crash.log", std::ios::app);
    log << "[FATAL] Server terminated due to unhandled exception!" << std::endl;
    std::abort(); // обязательно, иначе программа "зависнет"
}




LONG WINAPI myUnhandledFilter(EXCEPTION_POINTERS* ExceptionInfo) {
    std::ofstream log("crash.log", std::ios::app);
    log << "[SEH] Exception code: 0x" << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}



#include <iostream>
#include <coroutine>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

using namespace std::chrono_literals;

// === Минимальный планировщик ===

std::queue<std::coroutine_handle<>> task_queue;
std::mutex queue_mutex;
std::condition_variable queue_cv;

void schedule(std::coroutine_handle<> h) {
    {
        std::lock_guard lock(queue_mutex);
        task_queue.push(h);
    }
    queue_cv.notify_one();
}

void run_loop() {
    while (true) {
        std::coroutine_handle<> h;
        {
            std::unique_lock lock(queue_mutex);
            queue_cv.wait(lock, [] { return !task_queue.empty(); });
            h = task_queue.front();
            task_queue.pop();
        }
        if (h)
            h.resume();
    }
}

// === Awaitable Sleep ===

struct SleepAwaitable {
    int ms;
    std::coroutine_handle<> continuation;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        continuation = h;
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            schedule(continuation);
        }).detach();
    }

    void await_resume() const noexcept {}
};

// === Корутина-задача ===

struct Task {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    Task(handle_type h) : coro(h) {}
    Task(const Task&) = delete;
    Task(Task&& t) noexcept : coro(t.coro) { t.coro = nullptr; }
    ~Task() { if (coro) coro.destroy(); }

    handle_type coro;

    struct promise_type {
        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// === Клиенты ===

Task clientA() {
    while (true) {
        std::cout << "11 " << std::this_thread::get_id() << ")\n";
        co_await SleepAwaitable{1000};
        std::cout << "12\n";
        co_await SleepAwaitable{500};
    }
}

Task clientB() {
    while (true) {
        std::cout << "21 " << std::this_thread::get_id() << ")\n";
        co_await SleepAwaitable{1500};
        std::cout << "22\n";
        co_await SleepAwaitable{500};
    }
}

// === main ===










int main() {
    std::locale::global(std::locale("Russian_Russia.866"));
    /*auto a = clientA();
    auto b = clientB();

    schedule(a.coro);
    schedule(b.coro);

    run_loop();
*/
    std::set_terminate(myTerminateHandler);
    SetUnhandledExceptionFilter(myUnhandledFilter);


    auto settings = ConverterJSON::getSettings();
    TelegaWay::base_way_dir = "D:\\F12\\" + settings.year + ".db";
    TelegaWay::base_f12_dir = "D:\\F12\\base.db";
    TelegaWay::work_year = settings.year;


    auto jobs = ConverterJSON::parseBackupJobs("Backup.json");

    SqlLogger::instance("log.db"); // инициализация

    if (settings.hideMode) {
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_HIDE);
    }

    Telega::year = settings.year;
    Telega::prd_base_dir = settings.prd_base_dir;
    Telega::prm_base_dir = settings.prm_base_dir;
    Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
    Telega::b_prm = Telega::getBases(Telega::TYPE::ISHOD);


    try {
        boost::asio::io_context io_context;
        [[maybe_unused]] auto work_guard = boost::asio::make_work_guard(io_context); // <--- !
        std::vector<std::thread> thread_pool;
        for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
            thread_pool.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        PeriodicTaskManager<TaskId> scheduler;
        asio_server::Interface::setYear(settings.year);
        search_server::SearchServer myServer(settings, io_context);
        asio_server::Interface::setSearchServer(&myServer);

        scheduler.addTask<FlushPendingTask>(TaskId::FlushPendingTask, io_context, std::chrono::seconds(5), &myServer);
        scheduler.addTask<PeriodicUpdateTask>(TaskId::PeriodicUpdateTask, io_context, std::chrono::seconds(settings.indTime), &myServer);

        for (const auto& group : jobs) {
            scheduler.addTask<BackupTask>(
                    TaskId::BackupTask,
                    io_context,
                    std::chrono::seconds(group.period_sec),
                    group.backup_dir,
                    group.targets
            );
        }
        using namespace std;
        [[maybe_unused]] auto asioServer = std::make_shared<asio_server::AsioServer>(io_context, settings.port);

        string command;

        while (true) {
            cout << "---Search Engine is started!---" << endl;
            cout << "----Search Engine work now!----" << endl;
            cout << "------------- Menu ------------" << endl;
            cout << "'1' for exit." << endl;
            cout << "-------------------------------" << endl;
            cout << "Enter the command: ";

            cin >> command;
            cin.clear();

            if (command == "1") {
                cout << "Stopping server..." << endl;
                io_context.stop();
                break;
            } else {
                cout << "Unknown command" << endl;
            }

        }

        for (auto& thread : thread_pool) {
            if (thread.joinable()) {
                thread.join();
            }
        }

    } catch (ConverterJSON::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    } catch (search_server::SearchServer::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    }

    std::cout << "---Bye, bye!---" << std::endl;
    system("pause");

    return 0;
}
