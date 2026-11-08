#include "SearchEngineWindowsService.h"

#include "Application/SearchEngineApplication.h"
#include "MyUtils/LogFile.h"

#ifdef _WIN32

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {

struct ServiceHost {
    SearchEngineOptions options;
    SearchEngineRuntimePaths paths;
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    HANDLE stop_event = nullptr;
    SearchEngineApplication* application = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> stopped_reported{false};
    DWORD checkpoint = 1;
};

ServiceHost* g_host = nullptr;

std::string serviceNameUtf8(const ServiceHost& host)
{
    const std::wstring& name = host.options.service_name;
    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        name.data(),
        static_cast<int>(name.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (required <= 0) {
        return "SearchEngineService";
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        name.data(),
        static_cast<int>(name.size()),
        result.data(),
        required,
        nullptr,
        nullptr
    );
    return result;
}

void reportWindowsEvent(ServiceHost& host, const std::string& message)
{
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, message.c_str(), -1, nullptr, 0
    );
    std::wstring wide(
        required > 0 ? static_cast<size_t>(required) : size_t(1),
        L'\0'
    );
    if (required > 0) {
        MultiByteToWideChar(
            CP_UTF8, 0, message.c_str(), -1, wide.data(), required
        );
    } else {
        wide = L"SearchEngineService startup error";
    }

    HANDLE source = RegisterEventSourceW(
        nullptr,
        host.options.service_name.c_str()
    );
    if (!source)
        return;
    LPCWSTR strings[] = {wide.c_str()};
    ReportEventW(
        source,
        EVENTLOG_ERROR_TYPE,
        0,
        0,
        nullptr,
        1,
        0,
        strings,
        nullptr
    );
    DeregisterEventSource(source);
}

void reportStatus(
    ServiceHost& host,
    DWORD state,
    DWORD win32_exit_code,
    DWORD wait_hint,
    DWORD checkpoint)
{
    if (!host.status_handle) {
        return;
    }

    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = win32_exit_code;
    status.dwServiceSpecificExitCode = 0;
    status.dwWaitHint = wait_hint;
    status.dwCheckPoint = checkpoint;
    status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP |
          SERVICE_ACCEPT_SHUTDOWN |
          SERVICE_ACCEPT_PRESHUTDOWN
        : 0;
    SetServiceStatus(host.status_handle, &status);
}

void reportStopped(ServiceHost& host, DWORD exit_code)
{
    if (host.stopped_reported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    reportStatus(host, SERVICE_STOPPED, exit_code, 0, 0);
}

DWORD WINAPI serviceControlHandler(
    DWORD control,
    DWORD,
    LPVOID,
    LPVOID context)
{
    auto& host = *static_cast<ServiceHost*>(context);
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN ||
        control == SERVICE_CONTROL_PRESHUTDOWN)
    {
        host.stop_requested.store(true, std::memory_order_release);
        SetEvent(host.stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

template<typename Future>
void waitWithPendingStatus(
    ServiceHost& host,
    Future& future,
    DWORD pending_state,
    DWORD wait_hint)
{
    while (future.wait_for(std::chrono::seconds(3)) !=
           std::future_status::ready)
    {
        reportStatus(
            host,
            pending_state,
            NO_ERROR,
            wait_hint,
            host.checkpoint++
        );
        if (host.stop_requested.load(std::memory_order_acquire) &&
            host.application)
        {
            host.application->requestStop();
        }
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    ServiceHost& host = *g_host;

    // SCM requires handler registration to be the first service operation.
    host.status_handle = RegisterServiceCtrlHandlerExW(
        host.options.service_name.c_str(),
        serviceControlHandler,
        &host
    );
    if (!host.status_handle) {
        return;
    }

    try {
    reportStatus(host, SERVICE_START_PENDING, NO_ERROR, 30000, host.checkpoint++);

    std::string path_error;
    if (!activateSearchEngineRuntimePaths(host.paths, path_error)) {
        reportWindowsEvent(host, path_error);
        std::error_code exists_error;
        const DWORD path_code =
            std::filesystem::exists(host.paths.data_dir, exists_error)
                ? ERROR_ACCESS_DENIED
                : ERROR_PATH_NOT_FOUND;
        reportStopped(host, path_code);
        return;
    }

    LogFile::setLogsDirectory(host.paths.logs);
    LogFile::ensureLogsDir();
    LG("[SERVICE] startup requested; name=", serviceNameUtf8(host));

    SearchEngineApplication application(host.options, host.paths);
    host.application = &application;

    std::packaged_task<bool()> start_task([&application]() {
        return application.start();
    });
    std::future<bool> start_result = start_task.get_future();
    std::thread start_thread(std::move(start_task));
    waitWithPendingStatus(
        host,
        start_result,
        SERVICE_START_PENDING,
        30000
    );
    const bool started = start_result.get();
    start_thread.join();

    if (!started) {
        const DWORD code = application.lastWin32Error() == NO_ERROR
            ? ERROR_GEN_FAILURE
            : application.lastWin32Error();
        LG("[SERVICE START ERROR] ", application.lastError());
        application.stop();
        host.application = nullptr;
        reportStopped(host, code);
        return;
    }

    if (!host.stop_requested.load(std::memory_order_acquire)) {
        reportStatus(host, SERVICE_RUNNING, NO_ERROR, 0, 0);
        LG("[SERVICE] running; name=", serviceNameUtf8(host));
        WaitForSingleObject(host.stop_event, INFINITE);
    }

    reportStatus(
        host,
        SERVICE_STOP_PENDING,
        NO_ERROR,
        30000,
        host.checkpoint++
    );
    application.requestStop();

    std::packaged_task<void()> stop_task([&application]() {
        application.stop();
    });
    std::future<void> stop_result = stop_task.get_future();
    std::thread stop_thread(std::move(stop_task));
    waitWithPendingStatus(
        host,
        stop_result,
        SERVICE_STOP_PENDING,
        30000
    );
    stop_result.get();
    stop_thread.join();

    const DWORD exit_code = application.exitCode() == 0
        ? NO_ERROR
        : (application.lastWin32Error() == NO_ERROR
            ? ERROR_GEN_FAILURE
            : application.lastWin32Error());
    LG(exit_code == NO_ERROR
        ? "[SERVICE] stopped cleanly; name="
        : "[SERVICE] stopped with an error; name=",
        serviceNameUtf8(host));
    host.application = nullptr;
    reportStopped(host, exit_code);
    } catch (const std::exception& exception) {
        host.application = nullptr;
        const std::string message =
            std::string("Service host failure: ") +
            exception.what();
        reportWindowsEvent(host, message);
        try { LogFile::getErrors().write(message); } catch (...) {}
        reportStopped(host, ERROR_GEN_FAILURE);
    } catch (...) {
        host.application = nullptr;
        reportWindowsEvent(host, "Service host failure: unknown exception");
        reportStopped(host, ERROR_GEN_FAILURE);
    }
}

} // namespace

int runSearchEngineWindowsService(
    const SearchEngineOptions& options,
    const SearchEngineRuntimePaths& paths)
{
    ServiceHost host;
    host.options = options;
    host.paths = paths;
    host.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!host.stop_event) {
        return static_cast<int>(GetLastError());
    }

    g_host = &host;
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(host.options.service_name.c_str()), serviceMain},
        {nullptr, nullptr}
    };
    const BOOL dispatched = StartServiceCtrlDispatcherW(table);
    const DWORD error = dispatched ? NO_ERROR : GetLastError();
    g_host = nullptr;
    CloseHandle(host.stop_event);

    if (!dispatched) {
        OutputDebugStringW(
            L"SearchEngineService: StartServiceCtrlDispatcherW failed\n"
        );
    }
    return static_cast<int>(error);
}

#else

int runSearchEngineWindowsService(
    const SearchEngineOptions&,
    const SearchEngineRuntimePaths&)
{
    return 2;
}

#endif
