//
// Created by Sg on 01.10.2023.
//
#pragma once

#ifdef assert
#undef assert
#endif

#include "utf8cpp/utf8/checked.h"

#include <string>
#include <codecvt>


#include <filesystem>
#include <list>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>

#include <string>


// Функция: wstring → utf-8 string через utf8cpp
std::string wstring_to_utf85(const std::wstring& ws);

std::string utf8_to_wstring(const std::wstring& ws);


#include <codecvt>
#include <locale>
namespace my
{
    inline std::string utf8(const std::wstring& ws)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(ws);
    }
}


class StartLog {
public:
    static StartLog& instance()
    {
        static StartLog s("startup.log");   // название можно поменять
        return s;
    }

    template<class... Args>
    void write(Args&&... args)
    {
        std::lock_guard<std::mutex> lk(m_);
        stream_ << timestamp();
        (stream_ << ... << std::forward<Args>(args)) << '\n';
        stream_.flush();
    }

private:
    explicit StartLog(const char* file)
            : stream_(file, std::ios::app)
    {
        write("=== ===  Запуск программы  === ===");
    }

    static std::string timestamp()
    {
        using namespace std::chrono;
        auto tp   = system_clock::now();
        std::time_t tt = system_clock::to_time_t(tp);
        auto ms   = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;

        std::ostringstream os;
        os << std::put_time(std::localtime(&tt), "[%F %T")
           << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
        return os.str();
    }

    std::ofstream stream_;
    std::mutex    m_;
};

#define LG(...)  StartLog::instance().write(__VA_ARGS__)


class Interface {
    Interface() = default;
public:
    static Interface& getInstance()
    {
        static Interface instance;
        return instance;
    }

    static std::list<std::wstring> getAllFilesFromDir(const std::string& dir, const std::list<std::string>& ext);
    static std::vector<std::wstring> getAllFilesFromDirs(const std::vector<std::string>& dirs, const std::vector<std::string>& ext = std::vector<std::string>{},
                                                       const std::vector<std::string> &excludeDirs = {});

    static std::list<std::wstring> getAllFilesFromDir2(const std::string &dir, const std::vector<std::string> &exts,
                                                       const std::vector<std::string> &excludeDirs = {});
};


