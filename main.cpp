#include "Application/SearchEngineApplication.h"
#include "Application/SearchEngineOptions.h"
#include "Application/SearchEngineWindowsService.h"
#include "MyUtils/LogFile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

HANDLE g_console_stop_event = nullptr;
HANDLE g_console_shutdown_complete_event = nullptr;

void terminateHandler()
{
    try {
        LogFile::getErrors().write(
            "[FATAL] Server terminated due to an unhandled C++ exception"
        );
    } catch (...) {
    }
    std::abort();
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    try {
        const unsigned long code = info && info->ExceptionRecord
            ? info->ExceptionRecord->ExceptionCode
            : 0;
        LogFile::getErrors().write("[SEH] Exception code: ", code);
    } catch (...) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI consoleControlHandler(DWORD control)
{
    switch (control) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_console_stop_event) {
            SetEvent(g_console_stop_event);
        }
        if ((control == CTRL_CLOSE_EVENT ||
             control == CTRL_LOGOFF_EVENT ||
             control == CTRL_SHUTDOWN_EVENT) &&
            g_console_shutdown_complete_event)
        {
            WaitForSingleObject(g_console_shutdown_complete_event, 30000);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

std::vector<std::wstring> commandLineArguments(int argc, wchar_t* argv[])
{
    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return arguments;
}

void wakeConsoleInput(std::thread& input_thread)
{
    if (!input_thread.joinable()) {
        return;
    }

    CancelSynchronousIo(input_thread.native_handle());

    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        INPUT_RECORD records[2]{};
        records[0].EventType = KEY_EVENT;
        records[0].Event.KeyEvent.bKeyDown = TRUE;
        records[0].Event.KeyEvent.wRepeatCount = 1;
        records[0].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        records[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
        records[1] = records[0];
        records[1].Event.KeyEvent.bKeyDown = FALSE;
        DWORD written = 0;
        WriteConsoleInputW(input, records, 2, &written);
    }
}

int runConsole(
    const SearchEngineOptions& options,
    const SearchEngineRuntimePaths& paths)
{
    SearchEngineApplication application(options, paths);

    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE shutdown_complete_event =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event || !shutdown_complete_event) {
        std::cerr << "CreateEventW failed: " << GetLastError() << '\n';
        if (stop_event)
            CloseHandle(stop_event);
        if (shutdown_complete_event)
            CloseHandle(shutdown_complete_event);
        return 1;
    }

    g_console_stop_event = stop_event;
    g_console_shutdown_complete_event = shutdown_complete_event;
    SetConsoleCtrlHandler(consoleControlHandler, TRUE);

    if (!application.start()) {
        std::cerr << "SearchEngine startup failed: "
                  << application.lastError() << '\n';
        SetConsoleCtrlHandler(consoleControlHandler, FALSE);
        g_console_stop_event = nullptr;
        g_console_shutdown_complete_event = nullptr;
        CloseHandle(stop_event);
        CloseHandle(shutdown_complete_event);
        return application.exitCode() == 0 ? 1 : application.exitCode();
    }

    std::thread input_thread;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD input_mode = 0;
    const bool interactive_input =
        input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &input_mode);
    const bool readable_input =
        input != INVALID_HANDLE_VALUE &&
        GetFileType(input) != FILE_TYPE_UNKNOWN;
    if (readable_input) {
        input_thread = std::thread([stop_event, interactive_input]() {
            std::string command;
            while (std::cin) {
                if (interactive_input) {
                    std::cout << "\n--- Search Engine running ---\n"
                              << "1 - exit\n> " << std::flush;
                }
                if (!(std::cin >> command)) {
                    return;
                }
                if (command == "1") {
                    SetEvent(stop_event);
                    return;
                }
            }
        });
    } else {
        LG("Console stdin is unavailable; waiting for a control signal");
    }

    WaitForSingleObject(stop_event, INFINITE);
    application.requestStop();
    application.stop();

    wakeConsoleInput(input_thread);
    if (input_thread.joinable()) {
        input_thread.join();
    }

    SetEvent(shutdown_complete_event);
    SetConsoleCtrlHandler(consoleControlHandler, FALSE);
    g_console_stop_event = nullptr;
    g_console_shutdown_complete_event = nullptr;
    CloseHandle(stop_event);
    CloseHandle(shutdown_complete_event);
    std::cout << "--- Bye ---\n";
    return application.exitCode();
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    const std::vector<std::wstring> arguments =
        commandLineArguments(argc, argv);

    SearchEngineOptions options;
    std::string error;
    if (!parseSearchEngineOptions(arguments, options, error)) {
        std::cerr << "ERROR: " << error << "\n\n" << searchEngineUsage();
        return 2;
    }
    if (options.help) {
        std::cout << searchEngineUsage();
        return 0;
    }

    SearchEngineRuntimePaths paths;
    try {
        paths = resolveSearchEngineRuntimePaths(
            options,
            searchEngineExecutablePath()
        );
    } catch (const std::exception& exception) {
        std::cerr << "Cannot resolve executable path: "
                  << exception.what() << '\n';
        return 2;
    }

    if (options.mode == SearchEngineLaunchMode::Service) {
        return runSearchEngineWindowsService(options, paths);
    }

    if (!activateSearchEngineRuntimePaths(paths, error)) {
        std::cerr << "Cannot activate runtime paths: " << error << '\n';
        return 2;
    }

    LogFile::setLogsDirectory(paths.logs);
    LogFile::ensureLogsDir();
    std::set_terminate(terminateHandler);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);

    return runConsole(options, paths);
}
