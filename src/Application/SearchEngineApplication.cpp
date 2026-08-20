#include "SearchEngineApplication.h"

#include "AsioServer/AsioServer.h"
#include "Auth/AuthRuntime.h"
#include "Auth/IssuerPublicKeyPath.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "Commands/GetTelegaWay/TelegaWay.h"
#include "ContextRuntime/ContextRuntime.h"
#include "FileWatcher/Commands/IndexCommands.h"
#include "FileWatcher/Commands/OpdateOpisBaseCommand/RecordProcessor.h"
#include "FileWatcher/FileEventDispatcher.h"
#include "JSON/ConverterJSON.h"
#include "MyUtils/LogFile.h"
#include "MyUtils/FileExtensionContract.h"
#include "MyUtils/OEMCase.h"
#include "MyUtils/SettingsPathContract.h"
#include "MyUtils/SqlLogger.h"
#include "MyUtils/Utf8Path.h"
#include "SearchServer/SearchServer.h"
#include "scheduler/DelayEventTickTask.h"
#include "scheduler/FlushPendingTask.h"
#include "scheduler/PeriodicIndexUpdateTask.h"
#include "scheduler/PeriodicTaskManager.h"
#include "scheduler/TaskID.h"

#include <Windows.h>

#include <chrono>
#include <clocale>
#include <filesystem>
#include <locale>
#include <stdexcept>
#include <utility>

using namespace std::chrono_literals;

namespace {

class StartupError : public std::runtime_error {
public:
    StartupError(unsigned long code, const std::string& message)
        : std::runtime_error(message), code_(code)
    {
    }

    [[nodiscard]] unsigned long code() const noexcept { return code_; }

private:
    unsigned long code_;
};

class StartupCancelled : public StartupError {
public:
    StartupCancelled()
        : StartupError(ERROR_CANCELLED, "startup cancelled by stop request")
    {
    }
};

void validateSettings(const search_server::Settings& settings)
{
    switch (settings.documentCatalogStorage) {
    case inverted_index::DocumentCatalogStorage::Memory:
    case inverted_index::DocumentCatalogStorage::SQLite:
        break;
    default:
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.document_catalog_storage must be memory or sqlite"
        );
    }
    if (settings.threadCount < 0 || settings.threadCount == 1) {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.thread_count must be 0 (automatic) or at least 2"
        );
    }
    if (settings.port <= 0 || settings.port > 65535) {
        throw StartupError(ERROR_INVALID_DATA, "config.port is outside 1..65535");
    }
    if (const auto paths =
            settings_path_contract::validateConfiguredIndexPaths(
                settings.indexRoots, settings.excludedSubtrees);
        !paths.ok)
    {
        throw StartupError(ERROR_INVALID_DATA, paths.error);
    }
    const file_extension_contract::Selection fileTypes{
        settings.indexedExtensions,
        settings.includeExtensionlessFiles};
    if (const auto errors =
            file_extension_contract::validateCanonicalSelection(fileTypes);
        !errors.empty())
    {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config indexed file types " + errors.front());
    }
    if (settings.year.empty()) {
        throw StartupError(ERROR_INVALID_DATA, "config.year is empty");
    }
    // prm_base_dir / prd_base_dir may be empty: empty disables that AutoPad source.
    if (settings.fileIndexingTimeoutSec < 10 ||
        settings.fileIndexingTimeoutSec > 600)
    {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.file_indexing_timeout_sec is outside 10..600"
        );
    }
    if (settings.batchReaderThreads < 1 ||
        settings.batchReaderThreads > 64)
    {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.batch_reader_threads is outside 1..64"
        );
    }
    if (settings.batchIndexerThreads < 0 ||
        settings.batchIndexerThreads > 256)
    {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.batch_indexer_threads must be 0 or inside 1..256"
        );
    }
    if (settings.batchQueueMemoryMb < 16 ||
        settings.batchQueueMemoryMb > 2048)
    {
        throw StartupError(
            ERROR_INVALID_DATA,
            "config.batch_queue_memory_mb is outside 16..2048"
        );
    }
}

void initializeLocale()
{
    setlocale(LC_ALL, "Russian_Russia.866");
    std::locale base_ru("Russian_Russia.866");
    std::locale combined(
        base_ru,
        std::locale::classic(),
        std::locale::numeric
    );
    std::locale::global(combined);
}

} // namespace

struct SearchEngineApplication::Runtime {
    search_server::Settings settings;
    std::unique_ptr<ContextRuntime> contexts;
    std::unique_ptr<search_server::SearchServer> search_server;
    std::unique_ptr<FileEventDispatcher> dispatcher;
    std::unique_ptr<PeriodicTaskManager<TaskId>> scheduler;
    std::shared_ptr<asio_server::AsioServer> asio_server;
};

SearchEngineApplication::SearchEngineApplication(
    SearchEngineOptions options,
    SearchEngineRuntimePaths paths)
    : options_(std::move(options)), paths_(std::move(paths))
{
}

SearchEngineApplication::~SearchEngineApplication()
{
    stop();
}

bool SearchEngineApplication::start()
{
    SearchEngineApplicationState expected =
        SearchEngineApplicationState::Stopped;
    if (!state_.compare_exchange_strong(
            expected,
            SearchEngineApplicationState::Starting))
    {
        setFailure(ERROR_INVALID_STATE, "application has already been started");
        return false;
    }

    std::unique_ptr<Runtime> pending_runtime;
    try {
        LogFile::setLogsDirectory(paths_.logs);
        LogFile::ensureLogsDir();
        LG("SearchEngine startup begin; data-dir=", paths_.data_dir.string());

        throwIfStopRequested();
        if (!std::filesystem::is_regular_file(paths_.settings)) {
            throw StartupError(
                ERROR_FILE_NOT_FOUND,
                "Settings.json was not found: " + paths_.settings.string()
            );
        }

        ConverterJSON::setInteractiveErrors(
            options_.mode == SearchEngineLaunchMode::Console
        );

        auto runtime = std::make_unique<Runtime>();
        Runtime* const pending = runtime.get();
        pending_runtime = std::move(runtime);
        pending->settings = ConverterJSON::getSettings(
            paths_.settings.string()
        );
        validateSettings(pending->settings);
        LG("Settings loaded and validated");

        throwIfStopRequested();
        initializeLocale();
        OEMCase::init(paths_.oem866.string());
        Telega::year = pending->settings.year;
        Telega::prd_base_dir = pending->settings.prd_base_dir;
        Telega::prm_base_dir = pending->settings.prm_base_dir;
        RecordProcessor::setDefaultDirs(
            Telega::archiveDbPathFor(Telega::TYPE::VHOD),
            Telega::archiveDbPathFor(Telega::TYPE::ISHOD),
            encoding::utf8_path_join(
                pending->settings.opis_base_dir,
                pending->settings.year + ".DB"),
            "PRM",
            "PRD"
        );

        TelegaWay::base_way_dir = encoding::utf8_path_join(
            pending->settings.f12_base_dir,
            pending->settings.year + ".db");
        TelegaWay::base_f12_dir = encoding::utf8_path_join(
            pending->settings.f12_base_dir,
            "base.db");
        TelegaWay::work_year = pending->settings.year;

        pending->contexts = std::make_unique<ContextRuntime>(
            pending->settings.threadCount
        );
        pending->contexts->start();
        LG("I/O contexts started");

        throwIfStopRequested();
        SqlLogger::instance(paths_.log_database.string());
        try {
            auth::AuthRuntime::instance().initialize(paths_.auth_clients);
            LG("Auth client store opened; path=", paths_.auth_clients.string());
            LG(
                "Auth issuer public key; path=",
                auth::ResolveIssuerPublicPemPath(paths_.auth_clients).string()
            );
        } catch (const std::exception& ex) {
            throw StartupError(
                ERROR_OPEN_FAILED,
                std::string("failed to open auth_clients.sqlite: ") + ex.what());
        }

        pending->search_server =
            std::make_unique<search_server::SearchServer>(
                pending->settings,
                pending->contexts->cpu_pool(),
                pending->contexts->commit()
            );
        LG("Search index opened");

        asio_server::Interface::setYear(pending->settings.year);
        asio_server::Interface::setSearchServer(
            pending->search_server.get(),
            asio_server::ProductionCommandPaths{
                pending->settings.tlg_send_root,
                pending->settings.razn_output_dir,
                pending->settings.opis_base_dir,
                paths_.prefix_map});

        throwIfStopRequested();
        pending->dispatcher = std::make_unique<FileEventDispatcher>(
            pending->settings.indexRoots,
            file_extension_contract::Selection{
                pending->settings.indexedExtensions,
                pending->settings.includeExtensionlessFiles},
            pending->settings.excludedSubtrees,
            pending->contexts->scheduler()
        );
        pending->scheduler =
            std::make_unique<PeriodicTaskManager<TaskId>>();

        pending->dispatcher->registerCommand(
            FileEvent::Removed,
            std::make_unique<RemoveFileCommand>(*pending->search_server)
        );
        pending->dispatcher->registerCommand(
            FileEvent::Added,
            std::make_unique<AddFileCommand<TaskId>>(
                *pending->search_server,
                *pending->scheduler,
                file_extension_contract::Selection{
                    pending->settings.indexedExtensions,
                    pending->settings.includeExtensionlessFiles},
                pending->settings.enablePrmShortContentAutodetect
            )
        );

        pending->scheduler->addTask<FlushPendingTask2>(
            TaskId::FlushPendingTask,
            pending->contexts->scheduler(),
            pending->contexts->cpu_pool().get_executor(),
            7s,
            *pending->dispatcher
        );
        pending->scheduler->addTask<PeriodicIndexUpdateTask>(
            TaskId::PeriodicUpdateTask,
            pending->contexts->scheduler(),
            pending->contexts->cpu_pool().get_executor(),
            std::chrono::seconds(pending->settings.indTime),
            pending->search_server.get(),
            pending->settings.scanOnStartup
        );
        pending->scheduler->addTask<DelayEventTickTask<TaskId>>(
            TaskId::DelayEventTickTask,
            pending->contexts->scheduler(),
            pending->contexts->cpu_pool().get_executor(),
            2s,
            *pending->scheduler
        );
        LG("File watchers and scheduler started");

        throwIfStopRequested();
        try {
            pending->asio_server =
                std::make_shared<asio_server::AsioServer>(
                    pending->contexts->net(),
                    pending->contexts->cpu_pool(),
                    static_cast<unsigned short>(pending->settings.port)
                );
        } catch (const boost::system::system_error& exception) {
            const unsigned long native_error =
                static_cast<unsigned long>(exception.code().value());
            throw StartupError(
                native_error == 0 ? ERROR_OPEN_FAILED : native_error,
                std::string("cannot bind ASIO listen port: ") +
                    exception.what()
            );
        }
        LG("ASIO port bound; server accepts connections");

        if (options_.mode == SearchEngineLaunchMode::Console &&
            pending->settings.hideConsoleWindow)
        {
            ShowWindow(GetConsoleWindow(), SW_HIDE);
            LG("Console hidden by config.hide_console_window");
        }

        runtime_ = std::move(pending_runtime);
        state_.store(
            SearchEngineApplicationState::Running,
            std::memory_order_release
        );
        exit_code_.store(0, std::memory_order_relaxed);
        LG("SearchEngine startup complete");
        return true;
    } catch (const StartupError& exception) {
        setFailure(exception.code(), exception.what());
    } catch (const ConverterJSON::myExp& exception) {
        setFailure(
            ERROR_INVALID_DATA,
            "cannot parse settings file: " + exception.file
        );
    } catch (const std::exception& exception) {
        setFailure(ERROR_GEN_FAILURE, exception.what());
    } catch (...) {
        setFailure(ERROR_GEN_FAILURE, "unknown startup exception");
    }

    if (pending_runtime) {
        runtime_ = std::move(pending_runtime);
    }
    shutdownRuntime();
    return false;
}

void SearchEngineApplication::requestStop()
{
    const bool first =
        !stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (!first) {
        return;
    }

    SearchEngineApplicationState current = state_.load();
    if (current == SearchEngineApplicationState::Running ||
        current == SearchEngineApplicationState::Starting)
    {
        state_.store(SearchEngineApplicationState::StopRequested);
    }
    stop_condition_.notify_all();
}

void SearchEngineApplication::wait()
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    stop_condition_.wait(lock, [this]() {
        return stop_requested_.load(std::memory_order_acquire);
    });
}

void SearchEngineApplication::stop()
{
    std::call_once(stop_once_, [this]() {
        requestStop();
        state_.store(SearchEngineApplicationState::Stopping);
        shutdownRuntime();
        if (exit_code_.load(std::memory_order_relaxed) == 0) {
            state_.store(SearchEngineApplicationState::Stopped);
        } else {
            state_.store(SearchEngineApplicationState::Failed);
        }
        stop_condition_.notify_all();
    });
}

void SearchEngineApplication::shutdownRuntime() noexcept
{
    std::unique_ptr<Runtime> runtime = std::move(runtime_);
    if (!runtime) {
        auth::AuthRuntime::instance().shutdown();
        SqlLogger::shutdown();
        return;
    }

    try {
        LG("Shutdown: stop accepting clients");
        if (runtime->asio_server) {
            runtime->asio_server->stop();
        }

        LG("Shutdown: stop file watchers and scheduler");
        if (runtime->dispatcher) {
            runtime->dispatcher->stopAll();
        }
        if (runtime->scheduler) {
            runtime->scheduler->stopAll();
        }
        if (runtime->search_server) {
            runtime->search_server->requestStop();
        }

        if (runtime->asio_server) {
            runtime->asio_server->wait();
        }
        if (runtime->scheduler) {
            runtime->scheduler->waitAll();
            runtime->scheduler->clear();
        }

        asio_server::Interface::shutdown();
        runtime->scheduler.reset();
        runtime->dispatcher.reset();

        LG("Shutdown: drain indexing and save SQLite index");
        if (runtime->search_server) {
            runtime->search_server->stop();
            runtime->search_server.reset();
        }

        auth::AuthRuntime::instance().shutdown();
        SqlLogger::shutdown();

        LG("Shutdown: stop CPU and I/O runtimes");
        if (runtime->contexts) {
            runtime->contexts->stop();
        }
        runtime->asio_server.reset();
        runtime->contexts.reset();
        LG("SearchEngine shutdown complete");
    } catch (const std::exception& exception) {
        exit_code_.store(1, std::memory_order_relaxed);
        last_win32_error_.store(ERROR_GEN_FAILURE);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = std::string("shutdown failed: ") + exception.what();
        }
        try { LogFile::getErrors().write(lastError()); } catch (...) {}
    } catch (...) {
        exit_code_.store(1, std::memory_order_relaxed);
        last_win32_error_.store(ERROR_GEN_FAILURE);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = "shutdown failed: unknown exception";
        }
        try { LogFile::getErrors().write(lastError()); } catch (...) {}
    }
}

void SearchEngineApplication::setFailure(
    unsigned long win32_error,
    std::string message)
{
    exit_code_.store(1, std::memory_order_relaxed);
    last_win32_error_.store(win32_error, std::memory_order_relaxed);
    state_.store(SearchEngineApplicationState::Failed);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = std::move(message);
    }
    try {
        LogFile::getErrors().write("SearchEngine startup error: " + lastError());
        LG("SearchEngine startup error: ", lastError());
    } catch (...) {
    }
}

void SearchEngineApplication::throwIfStopRequested() const
{
    if (stop_requested_.load(std::memory_order_acquire)) {
        throw StartupCancelled();
    }
}

SearchEngineApplicationState SearchEngineApplication::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

bool SearchEngineApplication::isRunning() const noexcept
{
    return state() == SearchEngineApplicationState::Running;
}

bool SearchEngineApplication::stopRequested() const noexcept
{
    return stop_requested_.load(std::memory_order_acquire);
}

int SearchEngineApplication::exitCode() const noexcept
{
    return exit_code_.load(std::memory_order_relaxed);
}

unsigned long SearchEngineApplication::lastWin32Error() const noexcept
{
    return last_win32_error_.load(std::memory_order_relaxed);
}

std::string SearchEngineApplication::lastError() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

const SearchEngineRuntimePaths& SearchEngineApplication::paths() const noexcept
{
    return paths_;
}
