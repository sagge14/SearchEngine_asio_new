#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

/// Унифицированный потокобезопасный файловый лог.
/// Все логи пишутся в папку logs/ (создаётся при первом использовании).
/// Структура папок: logs/YYYY/MM/dd.MM.YYYY/name.log
class LogFile
{
public:
    /// Override the log root before the first log object is requested.
    /// The default remains the historical relative "logs" directory.
    static void setLogsDirectory(const std::filesystem::path& directory);
    static std::filesystem::path logsDirectory();

    /// Создать каталог logs/ (вызывается автоматически при первом открытии лога).
    static void ensureLogsDir();

    /// Доступ к логам по назначению.
    static LogFile& getStartup();
    static LogFile& getWatcher();
    static LogFile& getIndex();
    static LogFile& getErrors();
    static LogFile& getBackup();
    static LogFile& getScan();
    static LogFile& getRecord();
    static LogFile& getPing();

    void write(const std::string& msg);
    void write(const std::wstring& msg);
    /// Чтобы временные std::wstring (например L"..." + path) не подхватывались шаблоном и не шли в ofstream.
    void write(std::wstring&& msg) { write(static_cast<const std::wstring&>(msg)); }

    /// Для LG(): произвольные аргументы, вывод как в поток.
    template<class... Args>
    void write(Args&&... args)
    {
        std::lock_guard<std::mutex> lk(m_);
        ensureCurrentFile();
        stream_ << timestamp();
        (stream_ << ... << std::forward<Args>(args)) << '\n';
        stream_.flush();
    }

    /// Очистить файл лога (например, при старте для index.log).
    void clear();

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

private:
    explicit LogFile(const std::string& name);
    std::filesystem::path path() const;
    static std::string timestamp();
    std::string toUtf8(const std::wstring& ws) const;
    void ensureCurrentFile();
    static std::string getDatePath();

    std::string   name_;
    std::filesystem::path currentPath_;
    std::ofstream stream_;
    std::mutex    m_;
};

#define LG(...) LogFile::getStartup().write(__VA_ARGS__)
