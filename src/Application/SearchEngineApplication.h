#pragma once

#include "Application/SearchEngineOptions.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

enum class SearchEngineApplicationState {
    Stopped,
    Starting,
    Running,
    StopRequested,
    Stopping,
    Failed
};

class SearchEngineApplication {
public:
    SearchEngineApplication(
        SearchEngineOptions options,
        SearchEngineRuntimePaths paths
    );
    ~SearchEngineApplication();

    SearchEngineApplication(const SearchEngineApplication&) = delete;
    SearchEngineApplication& operator=(const SearchEngineApplication&) = delete;

    bool start();
    void requestStop();
    void wait();
    void stop();

    [[nodiscard]] SearchEngineApplicationState state() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] int exitCode() const noexcept;
    [[nodiscard]] unsigned long lastWin32Error() const noexcept;
    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] const SearchEngineRuntimePaths& paths() const noexcept;

private:
    struct Runtime;

    void shutdownRuntime() noexcept;
    void setFailure(unsigned long win32_error, std::string message);
    void throwIfStopRequested() const;

    SearchEngineOptions options_;
    SearchEngineRuntimePaths paths_;
    std::unique_ptr<Runtime> runtime_;

    std::atomic<SearchEngineApplicationState> state_{
        SearchEngineApplicationState::Stopped
    };
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> exit_code_{0};
    std::atomic<unsigned long> last_win32_error_{0};
    mutable std::mutex state_mutex_;
    std::condition_variable stop_condition_;
    std::string last_error_;
    std::once_flag stop_once_;
};
