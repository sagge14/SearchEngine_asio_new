#include "LogFile.h"
#include "Encoding.h"
#include <filesystem>

namespace {
    std::mutex g_logsDirectoryMutex;
    std::filesystem::path g_logsDirectory{"logs"};
}

void LogFile::setLogsDirectory(const std::filesystem::path& directory)
{
    std::lock_guard<std::mutex> lock(g_logsDirectoryMutex);
    g_logsDirectory = directory.empty()
        ? std::filesystem::path{"logs"}
        : directory;
}

std::filesystem::path LogFile::logsDirectory()
{
    std::lock_guard<std::mutex> lock(g_logsDirectoryMutex);
    return g_logsDirectory;
}

void LogFile::ensureLogsDir()
{
    std::error_code ec;
    std::filesystem::create_directories(logsDirectory(), ec);
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

std::string LogFile::getDatePath()
{
    using namespace std::chrono;
    auto tp = system_clock::now();
    std::time_t tt = system_clock::to_time_t(tp);
    std::tm* tm = std::localtime(&tt);
    
    // Используем C локаль для гарантии правильного форматирования чисел
    std::ostringstream os;
    os.imbue(std::locale::classic());  // C локаль - без форматирования тысяч
    
    // Формат: YYYY/MM/dd.MM.YYYY
    int year = tm->tm_year + 1900;
    int month = tm->tm_mon + 1;
    int day = tm->tm_mday;
    
    os << std::setfill('0')
       << year << '/'
       << std::setw(2) << month << '/'
       << std::setw(2) << day << '.'
       << std::setw(2) << month << '.'
       << year;
    return os.str();
}

std::string LogFile::toUtf8(const std::wstring& ws) const
{
    return encoding::wstring_to_utf8(ws);
}

LogFile::LogFile(const std::string& name) : name_(name), currentPath_()
{
    ensureCurrentFile();
}

std::filesystem::path LogFile::path() const
{
    return logsDirectory() / getDatePath() / (name_ + ".log");
}

void LogFile::ensureCurrentFile()
{
    const std::filesystem::path newPath = path();
    
    // Если путь изменился (сменилась дата) или файл не открыт, переоткрываем файл
    if (currentPath_ != newPath || !stream_.is_open()) {
        if (stream_.is_open()) {
            stream_.close();
        }
        
        // Создаём структуру папок: logs/YYYY/MM/dd.MM.YYYY/
        std::error_code ec;
        std::filesystem::create_directories(newPath.parent_path(), ec);
        stream_.open(newPath, std::ios::app);
        // Фиксируем C locale, чтобы не появлялись нестандартные разделители тысяч/десятичные.
        stream_.imbue(std::locale::classic());
        currentPath_ = newPath;

        // Однократный отметочный лог — помогает понять, перезаписывается ли файл
        // или вы смотрите на устаревшую версию.
        if (stream_.is_open()) {
            stream_ << timestamp()
                    << "LogFile opened: name=" << name_
                    << " path=" << newPath.string()
                    << " mode=app"
                    << '\n';
            stream_.flush();
        }
    }
}

void LogFile::clear()
{
    std::lock_guard<std::mutex> lk(m_);
    ensureCurrentFile();
    stream_.close();
    stream_.open(currentPath_, std::ios::trunc);
    stream_.close();
    stream_.open(currentPath_, std::ios::app);
}

void LogFile::write(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(m_);
    ensureCurrentFile();
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
