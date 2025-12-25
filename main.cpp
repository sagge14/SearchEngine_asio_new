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
#include "scheduler/DelayEventTickTask.h"
#include <windows.h>
#include "FileWatcher/Commands/OpdateOpisBaseCommand/RecordProcessor.h"



#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>

void rawLog(const std::string& msg)
{
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);

    std::ofstream f("io_ping.log", std::ios::app);
    if (!f) return;

    auto now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&now));

    f << "[" << buf << "] " << msg << "\n";
}


#include <iostream>

void pingIoRaw(boost::asio::io_context& ctx, const char* name)
{
    std::cout << "[PING post -> " << name << "]" << std::endl;

    boost::asio::post(ctx, [name]() {
        std::cout << "[PING executed <- " << name << "]" << std::endl;
    });
}



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
#include "OEMtoCase.h"
#include <memory>  // для std::unique_ptr/free


#include "Interface.h"



#define LG(...)  StartLog::instance().write(__VA_ARGS__)


void clearIndexingDebugLog() {
    std::ofstream log("indexing_debug.log");
    log << "== Новый запуск индексации ==" << std::endl;
}


int main() {
 //   RecordProcessor rp_{"D:\\BASES\\ARCHIVE.DB3","D:\\BASES_PRD\\ARCHIVE.DB3","D:\\OPIS_ADMIN\\2025.DB","PRM","PRD"};
 //   rp_.run(Telega::TYPE::VHOD,22133);

    auto settings = ConverterJSON::getSettings();
    LG("Settings loaded");

    clearIndexingDebugLog();
    LG("clearIndexingDebugLog");

    OEMtoCase::init("OEM866.INI");
    OEMtoUpper::init("OEM866.INI");

    LG("OEMtoCase::init DONE");

    RecordProcessor::setDefaultDirs("D:\\BASES\\ARCHIVE.DB3","D:\\BASES_PRD\\ARCHIVE.DB3","D:\\OPIS_ADMIN\\2025.DB","PRM","PRD");
    LG("RecordProcessor::setDefaultDirs DONE");

    setlocale(LC_ALL, "Russian_Russia.866");
    std::locale::global(std::locale("Russian_Russia.866"));
    LG("std::locale::global DONE");

    std::set_terminate(myTerminateHandler);
    LG("std::set_terminate DONE");

    SetUnhandledExceptionFilter(myUnhandledFilter);
    LG("SetUnhandledExceptionFilter DONE");

    boost::asio::io_context io_context_1;
    boost::asio::io_context io_context_2;
    boost::asio::io_context io_context_3;
    boost::asio::io_context io_context_4;

    auto work_guard_1 = boost::asio::make_work_guard(io_context_1);
    auto work_guard_2 = boost::asio::make_work_guard(io_context_2);
    auto work_guard_3 = boost::asio::make_work_guard(io_context_3);
    auto work_guard_4 = boost::asio::make_work_guard(io_context_4);

    LG("work guards created");


    std::vector<std::thread> thread_pool;

    size_t hw = std::thread::hardware_concurrency();
    size_t th_size = std::max<size_t>(settings.threadCount, hw);

// минимум 1 поток на каждый контекст
    size_t base = std::max<size_t>(1, th_size / 4);
    size_t th1 = base;                 // сеть
    size_t th2 = base;                 // логика
    size_t th3 = base;                 // индекс
    size_t th4 = th_size - th1 - th2 - th3; // commit

    if (th4 == 0) {
        th4 = 1;
        if (th3 > 1)      --th3;
        else if (th2 > 1) --th2;
        else              --th1;
    }

    LG("io_context_1 threads: ", th1);
    LG("io_context_2 threads: ", th2);
    LG("io_context_3 threads: ", th3);
    LG("io_context_4 threads: ", th4);



// io_context_1 — сеть
    for (size_t i = 0; i < th1; ++i)
    {
        thread_pool.emplace_back([&]() {
            io_context_1.run();
        });
    }

// io_context_2 — scheduler / watcher
    for (size_t i = 0; i < th2; ++i)
    {
        thread_pool.emplace_back([&]() {
            io_context_2.run();
        });
    }

// io_context_3 — индекс / fileIndexing
    for (size_t i = 0; i < th3; ++i)
    {
        thread_pool.emplace_back([&]() {
            io_context_3.run();
        });
    }

// io_context_4 — commit / strand
    for (size_t i = 0; i < th4; ++i)
    {
        thread_pool.emplace_back([&]() {
            io_context_4.run();
        });
    }

    LG("All io_context threads started");



    std::atomic<bool> pingRunning{false};

    std::thread pingThread([&]() {
        while (pingRunning.load())
        {
            pingIoRaw(io_context_1, "io_context_4");
            pingIoRaw(io_context_3, "io_context_3");

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });


    LG("All threads started");




    if (settings.hideMode) {
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_HIDE);
        LG("Console hidden (hideMode)");
    }

    TelegaWay::base_way_dir = "D:\\F12\\" + settings.year + ".db";
    LG("TelegaWay::base_way_dir set");
    TelegaWay::base_f12_dir = "D:\\F12\\base.db";
    LG("TelegaWay::base_f12_dir set");
    TelegaWay::work_year = settings.year;
    LG("TelegaWay::work_year set");

    SqlLogger::instance("log.db");
    LG("SqlLogger::instance DONE");

    Telega::year = settings.year;
    LG("Telega::year set");
    Telega::prd_base_dir = settings.prd_base_dir;
    LG("Telega::prd_base_dir set");
    Telega::prm_base_dir = settings.prm_base_dir;
    LG("Telega::prm_base_dir set");
    Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
    LG("Telega::b_prm VHOD set");
    Telega::b_prm = Telega::getBases(Telega::TYPE::ISHOD);
    LG("Telega::b_prm ISHOD set");

    asio_server::Interface::setYear(settings.year);
    LG("asio_server::Interface::setYear DONE");

    search_server::SearchServer myServer(settings, io_context_3, io_context_4);
    LG("SearchServer constructed");
    asio_server::Interface::setSearchServer(&myServer);
    LG("asio_server::Interface::setSearchServer DONE");

    PeriodicTaskManager<TaskId> scheduler;
    LG("PeriodicTaskManager constructed");

    FileEventDispatcher fileEventDispatcher{settings.dirs,settings.extensions, io_context_2};
    LG("FileEventDispatcher constructed");
    fileEventDispatcher.registerCommand(FileEvent::Added,std::make_unique<AddFileCommand<TaskId>>(myServer, scheduler,settings.extensions));
    LG("registerCommand: FileEvent::Added DONE");
    fileEventDispatcher.registerCommand(FileEvent::Removed,std::make_unique<RemoveFileCommand>(myServer));
    LG("registerCommand: FileEvent::Removed DONE");


    scheduler.addTask<FlushPendingTask2>(TaskId::FlushPendingTask, io_context_2, std::chrono::seconds(7), fileEventDispatcher);
    LG("scheduler: FlushPendingTask2 added");
    scheduler.addTask<PeriodicUpdateTask>(TaskId::PeriodicUpdateTask, io_context_2, std::chrono::seconds(settings.indTime), &myServer);
    LG("scheduler: PeriodicUpdateTask added");
    scheduler.addTask<DelayEventTickTask<TaskId>>(
            TaskId::DelayEventTickTask,
            io_context_2,
            std::chrono::seconds(2),
            scheduler // или &scheduler, если так надо по сигнатуре конструктора
    );
    LG("scheduler: DelayEventTickTask added");



    auto jobsForBackup = ConverterJSON::parseBackupJobs("Backup.json");
    LG("Backup jobs parsed");
    for (const auto& group : jobsForBackup) {
        scheduler.addTask<BackupTask>(
                TaskId::BackupTask,
                io_context_2,
                std::chrono::seconds(group.period_sec),
                group.backup_dir,
                group.targets
        );
        LG("BackupTask added: ", group.backup_dir);
    }

    [[maybe_unused]] auto asioServer = std::make_shared<asio_server::AsioServer>(io_context_1, settings.port);
    LG("asio_server::AsioServer created");
    //myServer.flushUpdateAndSaveDictionary();
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
                LG("fileEventDispatcher stopped");
                scheduler.stopAll();
                LG("scheduler stopped");
                break;
            }
            if (command == "2") {
                cout << "Stopping server..." << endl;
                myServer.dictionaryToLog();
                break;
            }
            else {
                cout << "Unknown command" << endl;
            }
        }

    } catch (ConverterJSON::myExp& e) {
        search_server::addToLog(std::string("Exception in commandExec: ") + e.what());
        LG("Exception in ConverterJSON::myExp: ", e.what());
    } catch (search_server::SearchServer::myExp& e) {
        search_server::addToLog(std::string("Exception in commandExec: ") + e.what());
        LG("Exception in SearchServer::myExp: ", e.what());
    }

    pingRunning = false;
    if (pingThread.joinable())
        pingThread.join();

    io_context_1.stop();
    io_context_2.stop();
    LG("io_context stopped");
    for (auto& thread : thread_pool) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    LG("Threads joined");
    std::cout << "---Bye, bye!---" << std::endl;
    system("pause");

    LG("main() end");
    return 0;
}

