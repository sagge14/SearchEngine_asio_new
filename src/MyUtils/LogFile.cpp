#include "LogFile.h"
#include "Encoding.h"
#include <filesystem>

namespace {
    std::once_flag g_logsDirOnce;
}

void LogFile::ensureLogsDir()
{
    std::call_once(g_logsDirOnce, []() {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
    });
}

std::string LogFile::timestamp()
{
    using namespace std::chrono;
    auto tp = system_clock::now();
    std::time_t tt = system_clock::to_time_t(tp);
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::ostringstream os;
    os << std::put_time(std::localtime(&tt), "[%F %T")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    return os.str();
}

std::string LogFile::toUtf8(const std::wstring& ws) const
{
    return encoding::wstring_to_utf8(ws);
}

LogFile::LogFile(const std::string& name) : name_(name)
{
    ensureLogsDir();
    stream_.open(path(), std::ios::app);
}

std::string LogFile::path() const
{
    return "logs/" + name_ + ".log";
}

void LogFile::clear()
{
    std::lock_guard<std::mutex> lk(m_);
    stream_.close();
    stream_.open(path(), std::ios::trunc);
    stream_.close();
    stream_.open(path(), std::ios::app);
}

void LogFile::write(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(m_);
    if (stream_.is_open()) {
        stream_ << timestamp() << msg << '\n';
        stream_.flush();
    }
}

void LogFile::write(const std::wstring& msg)
{
    write(toUtf8(msg));
}

// Статические экземпляры создаём внутри геттеров (членов класса), чтобы иметь доступ к private-конструктору.
LogFile& LogFile::getStartup()  { static LogFile l("startup");  return l; }
LogFile& LogFile::getWatcher() { static LogFile l("watcher"); return l; }
LogFile& LogFile::getIndex()   { static LogFile l("index");   return l; }
LogFile& LogFile::getErrors()  { static LogFile l("errors");  return l; }
LogFile& LogFile::getBackup()  { static LogFile l("backup");  return l; }
LogFile& LogFile::getScan()    { static LogFile l("scan");    return l; }
LogFile& LogFile::getRecord()  { static LogFile l("record");  return l; }
LogFile& LogFile::getPing()    { static LogFile l("ping");    return l; }
