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

std::string LogFile::path() const
{
    std::string datePath = getDatePath();
    return "logs/" + datePath + "/" + name_ + ".log";
}

void LogFile::ensureCurrentFile()
{
    std::string newPath = path();
    
    // Если путь изменился (сменилась дата) или файл не открыт, переоткрываем файл
    if (currentPath_ != newPath || !stream_.is_open()) {
        if (stream_.is_open()) {
            stream_.close();
        }
        
        // Создаём структуру папок: logs/YYYY/MM/dd.MM.YYYY/
        std::error_code ec;
#ifdef _WIN32
        // На Windows создаем путь через wstring для корректной обработки UTF-8
        std::wstring wPath = encoding::utf8_to_wstring(newPath);
        std::filesystem::path fsPath(wPath);
        std::filesystem::create_directories(fsPath.parent_path(), ec);
        // Открываем файл через wstring путь
        stream_.open(fsPath, std::ios::app);
#else
        std::filesystem::path fsPath(newPath);
        std::filesystem::create_directories(fsPath.parent_path(), ec);
        stream_.open(newPath, std::ios::app);
#endif
        currentPath_ = newPath;
    }
}

void LogFile::clear()
{
    std::lock_guard<std::mutex> lk(m_);
    ensureCurrentFile();
    stream_.close();
#ifdef _WIN32
    std::wstring wPath = encoding::utf8_to_wstring(currentPath_);
    stream_.open(wPath, std::ios::trunc);
    stream_.close();
    stream_.open(wPath, std::ios::app);
#else
    stream_.open(currentPath_, std::ios::trunc);
    stream_.close();
    stream_.open(currentPath_, std::ios::app);
#endif
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
