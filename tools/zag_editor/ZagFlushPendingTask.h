#pragma once

#include "scheduler/AbstractScheduledTask.h"

#include "FileWatcher/FileEventDispatcher.h"
#include "ZagEditorLog.h"

// Local flush task for ZagEditor:
// The original src/scheduler/FlushPendingTask.h includes SearchServer headers,
// which pulls unrelated dependencies (Boost.Serialization). For the standalone tool
// we only need to call FileEventDispatcher::flushPending().
namespace zag_editor {

class ZagFlushPendingTask final : public AbstractScheduledTask {
    FileEventDispatcher& dispatcher_;
    MinuteThrottledLogger& logger_;

public:
    ZagFlushPendingTask(boost::asio::io_context& io,
                         boost::asio::any_io_executor cpu_ex,
                         std::chrono::seconds period,
                         FileEventDispatcher& fed,
                         MinuteThrottledLogger& log)
        : AbstractScheduledTask(io, std::move(cpu_ex), period)
        , dispatcher_(fed)
        , logger_(log) {}

    void runTask() override {
        dispatcher_.flushPending();
        logger_.flush();
    }
};

} // namespace zag_editor

