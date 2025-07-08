#pragma once
#include "FileWatcher/MultiWatcher.h"
#include "Commands/IFileEventCommand.h"
#include <queue>

namespace boost::asio {
    class io_context;
}

class FileEventDispatcher {

public:

    using CommandMap = std::unordered_map<FileEvent, std::unique_ptr<IFileEventCommand>>;

    explicit FileEventDispatcher(const std::vector<std::string>& dirs, const std::vector<std::string>& ext,
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

    [[nodiscard]] bool matchByExtensions(const std::wstring& path) const;
    void initWatchers(const std::vector<std::string>& dirs);
    void pushFileEvent(FileEvent evt,  const std::wstring& path);

    std::vector<std::string> dirs_;
    std::vector<std::string> ext_;
    std::unordered_map<size_t, EventState> evtMap_;
    std::queue<size_t> pendingQ_;
    std::mutex mtx_;
    CommandMap commands_;
    boost::asio::io_context& io_;
    std::vector<std::unique_ptr<MultiDirWatcher>> dirWatchers_;
};
