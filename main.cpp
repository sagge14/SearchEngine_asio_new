// ============================================================================
// main.cpp — Search Server entry point
// ============================================================================


// ============================================================================
// STL
// ============================================================================
#include <iostream>
#include <thread>
#include <string>
#include <fstream>
#include <exception>
#include <memory>
#include <mutex>
#include <ctime>


// ============================================================================
// Windows networking (REQUIRED before windows.h)
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

// ============================================================================
// Platform / OS
// ============================================================================
#include <windows.h>

// ============================================================================
// Boost / Asio
// ============================================================================
#include <boost/asio.hpp>


// ============================================================================
// Infrastructure / Utils
// ============================================================================
#include "ContextRuntime/ContextRuntime.h"
#include "JSON/ConverterJSON.h"
#include "MyUtils/SqlLogger.h"
#include "MyUtils/OEMCase.h"
#include "MyUtils/LogFile.h"


// ============================================================================
// Search Server
// ============================================================================
#include "SearchServer/SearchServer.h"
#include "AsioServer/AsioServer.h"


// ============================================================================
// FileWatcher
// ============================================================================
#include "FileWatcher/FileEventDispatcher.h"
#include "FileWatcher/Commands/IndexCommands.h"
#include "FileWatcher/Commands/OpdateOpisBaseCommand/RecordProcessor.h"


// ============================================================================
// Telega / Business logic
// ============================================================================
#include "Commands/GetJsonTelega/Telega.h"
#include "Commands/GetTelegaWay/TelegaWay.h"


// ============================================================================
// Scheduler / Tasks
// ============================================================================
#include "scheduler/PeriodicTaskManager.h"
#include "scheduler/TaskID.h"
#include "scheduler/PeriodicIndexUpdateTask.h"
#include "scheduler/FlushPendingTask.h"
#include "scheduler/BackupTask.h"
#include "scheduler/DelayEventTickTask.h"


// ============================================================================
// Using / Macros
// ============================================================================
using namespace std::chrono_literals;



// ============================================================================
// Debug / Raw logging
// ============================================================================
void rawLog(const std::string& msg)
{
    LogFile::getPing().write(msg);
}

void pingIoRaw(boost::asio::io_context& ctx, const char* name)
{
    std::cout << "[PING post -> " << name << "]\n";
    boost::asio::post(ctx, [name]() {
        std::cout << "[PING executed <- " << name << "]\n";
    });
}


// ============================================================================
// Crash / Termination handlers
// ============================================================================
void myTerminateHandler()
{
    LogFile::ensureLogsDir();
    std::ofstream log("logs/errors.log", std::ios::app);
    if (log) log << "[FATAL] Server terminated due to unhandled exception!\n";
    std::abort();
}

LONG WINAPI myUnhandledFilter(EXCEPTION_POINTERS* info)
{
    LogFile::ensureLogsDir();
    std::ofstream log("logs/errors.log", std::ios::app);
    if (log) log << "[SEH] Exception code: 0x"
        << std::hex << info->ExceptionRecord->ExceptionCode << "\n";
    return EXCEPTION_EXECUTE_HANDLER;
}


// ============================================================================
// Helpers
// ============================================================================
void clearIndexingDebugLog()
{
    LogFile::getIndex().clear();
    LogFile::getIndex().write("== New indexing session started ==");
}


// ============================================================================
// main()
// ============================================================================
int main()
{
    LogFile::ensureLogsDir();

    // ------------------------------------------------------------------------
    // Load settings
    // ------------------------------------------------------------------------
    auto settings = ConverterJSON::getSettings();
    LG("Settings loaded");

    clearIndexingDebugLog();
    LG("Indexing debug log cleared");

    // ------------------------------------------------------------------------
    // Platform / locale init
    // ------------------------------------------------------------------------
    OEMCase::init("OEM866.INI");

    RecordProcessor::setDefaultDirs(
            "D:\\BASES\\ARCHIVE.DB3",
            "D:\\BASES_PRD\\ARCHIVE.DB3",
            "D:\\OPIS_ADMIN\\" + settings.year + ".DB",
            "PRM",
            "PRD"
    );

    setlocale(LC_ALL, "Russian_Russia.866");
    std::locale::global(std::locale("Russian_Russia.866"));

    std::set_terminate(myTerminateHandler);
    SetUnhandledExceptionFilter(myUnhandledFilter);

    LG("Platform initialized");

    // ------------------------------------------------------------------------
    // IO runtime
    // ------------------------------------------------------------------------
    ContextRuntime runtime(settings.threadCount);
    runtime.start();
    LG("IO contexts started");

    // ------------------------------------------------------------------------
    // Console mode
    // ------------------------------------------------------------------------
    if (settings.hideMode) {
        ShowWindow(GetConsoleWindow(), SW_HIDE);
        LG("Console hidden");
    }

    // ------------------------------------------------------------------------
    // Telega init
    // ------------------------------------------------------------------------
    TelegaWay::base_way_dir = "D:\\F12\\" + settings.year + ".db";
    TelegaWay::base_f12_dir = "D:\\F12\\base.db";
    TelegaWay::work_year   = settings.year;

    Telega::year          = settings.year;
    Telega::prd_base_dir  = settings.prd_base_dir;
    Telega::prm_base_dir  = settings.prm_base_dir;

    LG("Telega initialized");

    // ------------------------------------------------------------------------
    // Logger
    // ------------------------------------------------------------------------
    SqlLogger::instance("log.db");
    LG("SqlLogger started");

    // ------------------------------------------------------------------------
    // Search server
    // ------------------------------------------------------------------------
    search_server::SearchServer server(
            settings,
            runtime.cpu_pool(),
            runtime.commit()
    );

    LG("SearchServer created");

    asio_server::Interface::setYear(settings.year);
    asio_server::Interface::setSearchServer(&server);

    // ------------------------------------------------------------------------
    // Scheduler & FileWatcher
    // ------------------------------------------------------------------------

    FileEventDispatcher dispatcher(
            settings.dirs,
            settings.extensions,
            runtime.scheduler()
    );

    PeriodicTaskManager<TaskId> scheduler;

    dispatcher.registerCommand(
            FileEvent::Removed,
            std::make_unique<RemoveFileCommand>(server)
    );

    dispatcher.registerCommand(
            FileEvent::Added,
            std::make_unique<AddFileCommand<TaskId>>(server, scheduler, settings.extensions)
    );


    scheduler.addTask<FlushPendingTask2>(
            TaskId::FlushPendingTask,
            runtime.scheduler(),
            runtime.cpu_pool().get_executor(),
            7s,
            dispatcher
    );

    scheduler.addTask<PeriodicIndexUpdateTask>(
            TaskId::PeriodicUpdateTask,
            runtime.scheduler(),
            runtime.cpu_pool().get_executor(),
            std::chrono::seconds(settings.indTime),
            &server
    );

    scheduler.addTask<DelayEventTickTask<TaskId>>(
            TaskId::DelayEventTickTask,
            runtime.scheduler(),
            runtime.cpu_pool().get_executor(),
            2s,
            scheduler
    );

    // ------------------------------------------------------------------------
    // Backup tasks
    // ------------------------------------------------------------------------
    auto jobs = ConverterJSON::parseBackupJobs("Backup.json");
    for (const auto& job : jobs) {
        scheduler.addTask<BackupTask>(
                TaskId::BackupTask,
                runtime.scheduler(),
                runtime.cpu_pool().get_executor(),
                std::chrono::seconds(job.period_sec),
                job.backup_dir,
                job.targets
        );
    }

    LG("Scheduler initialized");
    LG("All file logs: logs/ (startup, watcher, index, errors, backup, scan, record, ping)");

    // ------------------------------------------------------------------------
    // Asio server
    // ------------------------------------------------------------------------
    auto asioServer = std::make_shared<asio_server::AsioServer>(
            runtime.net(),
            runtime.cpu_pool(),
            settings.port
    );

    LG("AsioServer started");

    // ------------------------------------------------------------------------
    // Console loop
    // ------------------------------------------------------------------------
    try {
        std::string cmd;
        while (true) {
            std::cout << "\n--- Search Engine running ---\n"
                      << "1 - exit\n> ";
            std::cin >> cmd;

            if (cmd == "1") {
                dispatcher.stopAll();
                scheduler.stopAll();
                break;
            }
        }
    }
    catch (const std::exception& e) {
        LG("Fatal exception: ", e.what());
    }

    // ------------------------------------------------------------------------
    // Shutdown
    // ------------------------------------------------------------------------
    runtime.stop();
    LG("Runtime stopped");

    std::cout << "--- Bye ---\n";
    system("pause");
    return 0;
}
