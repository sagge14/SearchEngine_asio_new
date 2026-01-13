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
#include "ContextRuntime.h"
#include "ConverterJSON.h"
#include "SqlLogger.h"
#include "OEMtoCase.h"
#include "Interface.h"


// ============================================================================
// Search Server
// ============================================================================
#include "SearchServer/SearchServer.h"
#include "AsioServer.h"


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
#include "scheduler/UpdateIndexTask.h"
#include "scheduler/PeriodicUpdateTask.h"
#include "scheduler/FlushPendingTask.h"
#include "scheduler/BackupTask.h"
#include "scheduler/DelayEventTickTask.h"


// ============================================================================
// Using / Macros
// ============================================================================
using namespace std::chrono_literals;

#define LG(...) StartLog::instance().write(__VA_ARGS__)


// ============================================================================
// Debug / Raw logging
// ============================================================================
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
    std::ofstream log("fatal_crash.log", std::ios::app);
    log << "[FATAL] Server terminated due to unhandled exception!\n";
    std::abort();
}

LONG WINAPI myUnhandledFilter(EXCEPTION_POINTERS* info)
{
    std::ofstream log("crash.log", std::ios::app);
    log << "[SEH] Exception code: 0x"
        << std::hex << info->ExceptionRecord->ExceptionCode << "\n";
    return EXCEPTION_EXECUTE_HANDLER;
}


// ============================================================================
// Helpers
// ============================================================================
void clearIndexingDebugLog()
{
    std::ofstream log("indexing_debug.log");
    log << "== Новый запуск индексации ==\n";
}


// ============================================================================
// main()
// ============================================================================
int main()
{
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
    OEMtoCase::init("OEM866.INI");
    OEMtoUpper::init("OEM866.INI");

    RecordProcessor::setDefaultDirs(
            "D:\\BASES\\ARCHIVE.DB3",
            "D:\\BASES_PRD\\ARCHIVE.DB3",
            "D:\\OPIS_ADMIN\\2025.DB",
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
    Telega::b_prm         = Telega::getBases(Telega::TYPE::ISHOD);

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
            runtime.index(),
            runtime.commit()
    );

    asio_server::Interface::setYear(settings.year);
    asio_server::Interface::setSearchServer(&server);

    LG("SearchServer created");

    // ------------------------------------------------------------------------
    // Scheduler & FileWatcher
    // ------------------------------------------------------------------------
    PeriodicTaskManager<TaskId> scheduler;

    FileEventDispatcher dispatcher(
            settings.dirs,
            settings.extensions,
            runtime.scheduler()
    );

    dispatcher.registerCommand(
            FileEvent::Added,
            std::make_unique<AddFileCommand<TaskId>>(server, scheduler, settings.extensions)
    );

    dispatcher.registerCommand(
            FileEvent::Removed,
            std::make_unique<RemoveFileCommand>(server)
    );

    scheduler.addTask<FlushPendingTask2>(
            TaskId::FlushPendingTask,
            runtime.scheduler(),
            7s,
            dispatcher
    );

    scheduler.addTask<PeriodicUpdateTask>(
            TaskId::PeriodicUpdateTask,
            runtime.scheduler(),
            std::chrono::seconds(settings.indTime),
            &server
    );

    scheduler.addTask<DelayEventTickTask<TaskId>>(
            TaskId::DelayEventTickTask,
            runtime.scheduler(),
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
                std::chrono::seconds(job.period_sec),
                job.backup_dir,
                job.targets
        );
    }

    LG("Scheduler initialized");

    // ------------------------------------------------------------------------
    // Asio server
    // ------------------------------------------------------------------------
    [[maybe_unused]]
    auto asioServer = std::make_shared<asio_server::AsioServer>(
            runtime.net(),
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
                      << "1 — exit\n> ";
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
