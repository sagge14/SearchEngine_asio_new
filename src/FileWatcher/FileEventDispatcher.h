#pragma once
#include "FileWatcher/MultiWatcher.h"
#include "Commands/IFileEventCommand.h"
#include <queue>
#include <atomic>

namespace boost::asio {
    class io_context;
}

class FileEventDispatcher {

public:

    using CommandMap = std::unordered_map<FileEvent, std::unique_ptr<IFileEventCommand>>;

    explicit FileEventDispatcher(const std::vector<std::string>& indexRoots,
                                 const std::vector<std::string>& extensions,
                                 const std::vector<std::string>& excludedSubtrees,
                                 boost::asio::io_context& io);
    void flushPending();

    void registerCommand(FileEvent evt, std::unique_ptr<IFileEventCommand> cmd) {
        commands_[evt] = (std::move(cmd));
    }
    void stopAll();

private:

    struct EventState {
        FileEvent evt;
        std::wstring path;
        bool queued{false};
    };

    void initWatchers(const std::vector<std::string>& indexRoots);
    void pushFileEvent(FileEvent evt,  const std::wstring& path);

    std::vector<std::string> indexRoots_;
    std::vector<std::string> ext_;
    std::vector<std::string> excludedSubtrees_;
    std::unordered_map<size_t, EventState> evtMap_;
    std::queue<size_t> pendingQ_;
    std::mutex mtx_;
    CommandMap commands_;
    boost::asio::io_context& io_;
    std::vector<std::unique_ptr<MultiDirWatcher>> dirWatchers_;
    std::atomic<bool> stopping_{false};
};
