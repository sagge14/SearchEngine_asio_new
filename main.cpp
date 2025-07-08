#include <iostream>
#include <thread>
#include <vector>
#include "FileWatcher/Commands/IndexCommands.h"
#include "FileWatcher/FileEventDispatcher.h"
#include "SearchServer/SearchServer.h"
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

using namespace std::chrono_literals;

int main() {

    std::set_terminate(myTerminateHandler);
    SetUnhandledExceptionFilter(myUnhandledFilter);
    std::locale::global(std::locale("Russian_Russia.866"));

    boost::asio::io_context io_context;
    [[maybe_unused]] auto work_guard = boost::asio::make_work_guard(io_context);
    std::vector<std::thread> thread_pool;
    auto th_size = std::thread::hardware_concurrency();
    for (size_t i = 0; i < th_size; ++i) {
        thread_pool.emplace_back([&io_context]() {
            io_context.run();
        });
    }

    auto settings = ConverterJSON::getSettings();

    if (settings.hideMode) {
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_HIDE);
    }

    TelegaWay::base_way_dir = "D:\\F12\\" + settings.year + ".db";
    TelegaWay::base_f12_dir = "D:\\F12\\base.db";
    TelegaWay::work_year = settings.year;

    SqlLogger::instance("log.db"); // инициализация

    Telega::year = settings.year;
    Telega::prd_base_dir = settings.prd_base_dir;
    Telega::prm_base_dir = settings.prm_base_dir;
    Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
    Telega::b_prm = Telega::getBases(Telega::TYPE::ISHOD);

    asio_server::Interface::setYear(settings.year);
    search_server::SearchServer myServer(settings, io_context);
    asio_server::Interface::setSearchServer(&myServer);

    FileEventDispatcher fileEventDispatcher{settings.dirs,settings.extensions, io_context};
    fileEventDispatcher.registerCommand(FileEvent::Added,std::make_unique<AddFileCommand>(myServer));
    fileEventDispatcher.registerCommand(FileEvent::Removed,std::make_unique<RemoveFileCommand>(myServer));

    PeriodicTaskManager<TaskId> scheduler;
    scheduler.addTask<FlushPendingTask2>(TaskId::FlushPendingTask, io_context, std::chrono::seconds(3), fileEventDispatcher);
    scheduler.addTask<PeriodicUpdateTask>(TaskId::PeriodicUpdateTask, io_context, std::chrono::seconds(settings.indTime), &myServer);

    auto jobsForBackup = ConverterJSON::parseBackupJobs("Backup.json");
    for (const auto& group : jobsForBackup) {
        scheduler.addTask<BackupTask>(
                TaskId::BackupTask,
                io_context,
                std::chrono::seconds(group.period_sec),
                group.backup_dir,
                group.targets
        );
    }

    [[maybe_unused]] auto asioServer = std::make_shared<asio_server::AsioServer>(io_context, settings.port);

    try {

        using namespace std;
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
                fileEventDispatcher.stopAll();
                scheduler.stopAll();
                break;
            } else {
                cout << "Unknown command" << endl;
            }

        }

    } catch (ConverterJSON::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    } catch (search_server::SearchServer::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    }

    io_context.stop();
    for (auto& thread : thread_pool) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    std::cout << "---Bye, bye!---" << std::endl;
    system("pause");

    return 0;
}
