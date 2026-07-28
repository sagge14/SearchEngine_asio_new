#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace zag_editor {

// minute-based throttling:
// - несколько сообщений в пределах одной минуты буферизуются в памяти
// - при смене минуты буфер один раз дописывается в файл этой минуты
// - логи старше retentionHours удаляются при каждом flush-вращении
class MinuteThrottledLogger {
public:
    explicit MinuteThrottledLogger(
            std::filesystem::path logDir = "logs/zag_editor",
            int retentionHours = 24);
    void setTeeToConsole(bool enabled) { teeToConsole_ = enabled; }

    ~MinuteThrottledLogger() { flush(); }

    void logInfo(const std::wstring& msg);
    void logError(const std::wstring& msg);

    void flush();

private:
    std::filesystem::path logDir_;
    int retentionHours_{24};
    bool teeToConsole_{true};

    std::mutex m_;
    std::string bufferMinuteKey_; // YYYY-MM-DD_HHMM
    std::string lastCleanupMinuteKey_; // YYYY-MM-DD_HHMM
    std::string lastWrittenMinuteKey_; // YYYY-MM-DD_HHMM
    std::vector<std::string> bufferLines_;

    std::string minuteKeyNow();
    std::string timestampNow();

    void writeBufferLocked();
    void cleanupOldLogsLocked();

    void logLineLocked(const std::string& line, const std::string& msgMinuteKey);
    static std::string trimNewlines(std::string s);
};

} // namespace zag_editor

