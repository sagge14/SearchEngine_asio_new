#include "BackupWindowsService.h"

#include "BackupServiceApplication.h"
#include "MyUtils/LogFile.h"

#ifdef _WIN32

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace {

struct ServiceHost {
    BackupServiceOptions options;
    BackupRuntimePaths paths;
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    HANDLE stop_event = nullptr;
    std::unique_ptr<BackupServiceApplication> application;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> stopped_reported{false};
    DWORD checkpoint = 1;
};

ServiceHost* g_host = nullptr;

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
    status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP |
              SERVICE_ACCEPT_SHUTDOWN |
              SERVICE_ACCEPT_PRESHUTDOWN
            : 0;
    SetServiceStatus(host.status_handle, &status);
}

void reportStopped(ServiceHost& host, DWORD exit_code)
{
    if (host.stopped_reported.exchange(true)) {
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
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        if (!host.stop_requested.exchange(true)) {
            reportStatus(
                host,
                SERVICE_STOP_PENDING,
                NO_ERROR,
                30000,
                host.checkpoint++
            );
            SetEvent(host.stop_event);
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    ServiceHost& host = *g_host;
    host.status_handle = RegisterServiceCtrlHandlerExW(
        host.options.service_name.c_str(),
        serviceControlHandler,
        &host
    );
    if (!host.status_handle) {
        return;
    }

    reportStatus(host, SERVICE_START_PENDING, NO_ERROR, 30000, 1);
    LogFile::setLogsDirectory(host.paths.logs);
    LogFile::ensureLogsDir();
    LogFile::getBackup().write(
        "[SERVICE] Starting Windows service instance"
    );

    host.application = std::make_unique<BackupServiceApplication>();
    std::string error;
    if (!host.application->configure(host.options, host.paths, error)) {
        LogFile::getBackup().write("[SERVICE START ERROR] " + error);
        reportStopped(host, ERROR_INVALID_DATA);
        return;
    }
    reportStatus(host, SERVICE_START_PENDING, NO_ERROR, 30000, 2);

    if (host.stop_requested.load()) {
        LogFile::getBackup().write(
            "[SERVICE] Stop requested during configuration"
        );
        reportStopped(host, NO_ERROR);
        return;
    }

    if (!host.application->start(error)) {
        LogFile::getBackup().write("[SERVICE START ERROR] " + error);
        reportStopped(host, ERROR_GEN_FAILURE);
        return;
    }

    reportStatus(host, SERVICE_RUNNING, NO_ERROR, 0, 0);
    LogFile::getBackup().write("[SERVICE] Service is running");

    while (!host.stop_requested.load()) {
        const DWORD wait_result = WaitForSingleObject(host.stop_event, 1000);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (!host.application->isRunning()) {
            host.stop_requested.store(true);
            break;
        }
    }

    reportStatus(
        host,
        SERVICE_STOP_PENDING,
        NO_ERROR,
        30000,
        host.checkpoint++
    );
    host.application->requestStop();

    while (!host.application->waitFor(std::chrono::seconds(5))) {
        reportStatus(
            host,
            SERVICE_STOP_PENDING,
            NO_ERROR,
            30000,
            host.checkpoint++
        );
    }
    host.application->wait();

    const int application_exit = host.application->exitCode();
    LogFile::getBackup().write(
        application_exit == 0
            ? "[SERVICE] Service stopped cleanly"
            : "[SERVICE] Service stopped after a runtime error"
    );
    reportStopped(
        host,
        application_exit == 0 ? NO_ERROR : ERROR_GEN_FAILURE
    );
}

} // namespace

int runBackupWindowsService(
    const BackupServiceOptions& options,
    const BackupRuntimePaths& paths)
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
        {
            host.options.service_name.data(),
            serviceMain
        },
        {nullptr, nullptr}
    };

    const BOOL dispatched = StartServiceCtrlDispatcherW(table);
    const DWORD error = dispatched ? NO_ERROR : GetLastError();
    g_host = nullptr;
    CloseHandle(host.stop_event);

    if (!dispatched) {
        LogFile::getBackup().write(
            "[SERVICE START ERROR] StartServiceCtrlDispatcherW failed: " +
            std::to_string(error)
        );
    }
    return static_cast<int>(error);
}

#else

int runBackupWindowsService(
    const BackupServiceOptions&,
    const BackupRuntimePaths&)
{
    return 2;
}

#endif
