#include "ZagEditorLog.h"

#include "MyUtils/Encoding.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace zag_editor {
namespace {

static std::string localTimeKeyYMDHM(std::chrono::system_clock::time_point tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d_%H%M");
    return os.str();
}

static std::string localTimeStamp(std::chrono::system_clock::time_point tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "[%F %T]");
    return os.str();
}

} // namespace

MinuteThrottledLogger::MinuteThrottledLogger(std::filesystem::path logDir, int retentionHours)
    : logDir_(std::move(logDir))
    , retentionHours_(retentionHours) {
    std::error_code ec;
    std::filesystem::create_directories(logDir_, ec);
}

std::string MinuteThrottledLogger::minuteKeyNow() {
    return localTimeKeyYMDHM(std::chrono::system_clock::now());
}

std::string MinuteThrottledLogger::timestampNow() {
    return localTimeStamp(std::chrono::system_clock::now());
}

std::string MinuteThrottledLogger::trimNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void MinuteThrottledLogger::logLineLocked(const std::string& line, const std::string& msgMinuteKey) {
    if (bufferMinuteKey_.empty()) {
        bufferMinuteKey_ = msgMinuteKey;
    }

    // Cleanup should happen at most once per minute, even if messages keep coming.
    if (msgMinuteKey != lastCleanupMinuteKey_) {
        cleanupOldLogsLocked();
        lastCleanupMinuteKey_ = msgMinuteKey;
    }

    // Rotate on minute boundary: only then we touch disk.
    if (msgMinuteKey != bufferMinuteKey_) {
        writeBufferLocked();
        bufferMinuteKey_ = msgMinuteKey;
    }

    bufferLines_.push_back(line);
}

void MinuteThrottledLogger::writeBufferLocked() {
    if (bufferMinuteKey_.empty() || bufferLines_.empty()) return;

    std::filesystem::path file = logDir_ / (bufferMinuteKey_ + ".log");
    std::ofstream out(file, std::ios::app);
    if (out.is_open()) {
        for (const auto& l : bufferLines_) {
            out << l << '\n';
        }
        out.flush();
    }

    lastWrittenMinuteKey_ = bufferMinuteKey_;
    bufferLines_.clear();
}

void MinuteThrottledLogger::cleanupOldLogsLocked() {
    if (retentionHours_ <= 0) return;

    namespace fs = std::filesystem;
    std::error_code ec;
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(retentionHours_);

    if (!fs::exists(logDir_, ec)) return;

    for (const auto& entry : fs::directory_iterator(logDir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto ftime = entry.last_write_time(ec);
        if (ec) continue;

        // Convert file time to system_clock time point.
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now()
                + std::chrono::system_clock::now());

        if (sctp < cutoff) {
            fs::remove(entry.path(), ec);
        }
    }
}

void MinuteThrottledLogger::logInfo(const std::wstring& msg) {
    std::lock_guard<std::mutex> lk(m_);
    const auto mk = minuteKeyNow();
    std::string utf8 = encoding::wstring_to_utf8(msg);
    utf8 = trimNewlines(utf8);
    std::string line = timestampNow() + " " + utf8;
    logLineLocked(line, mk);
    if (teeToConsole_) {
        std::cout << line << std::endl;
    }
}

void MinuteThrottledLogger::logError(const std::wstring& msg) {
    std::lock_guard<std::mutex> lk(m_);
    const auto mk = minuteKeyNow();
    std::string utf8 = encoding::wstring_to_utf8(msg);
    utf8 = trimNewlines(utf8);
    std::string line = timestampNow() + " [ERROR] " + utf8;
    logLineLocked(line, mk);
    if (teeToConsole_) {
        std::cerr << line << std::endl;
    }
}

void MinuteThrottledLogger::flush() {
    std::lock_guard<std::mutex> lk(m_);
    if (bufferMinuteKey_.empty() || bufferLines_.empty()) return;

    // Throttle disk writes: at most once per minute file.
    const auto currentKey = minuteKeyNow();
    if (lastWrittenMinuteKey_ == currentKey) return;

    // Writes buffered lines into <currentKey>.log (same bufferMinuteKey_).
    // If bufferMinuteKey_ is stale (minute changed but we didn't rotate yet),
    // the writeBufferLocked() still uses bufferMinuteKey_ as file name.
    writeBufferLocked();
    cleanupOldLogsLocked();
}

} // namespace zag_editor

