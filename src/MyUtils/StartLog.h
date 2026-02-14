#pragma once
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

class StartLog
{
public:
    static StartLog& instance();

    template<class... Args>
    void write(Args&&... args)
    {
        std::lock_guard<std::mutex> lk(m_);
        stream_ << timestamp();
        (stream_ << ... << std::forward<Args>(args)) << '\n';
    }

private:
    explicit StartLog(const char* file);

    static std::string timestamp();

    std::ofstream stream_;
    std::mutex    m_;
};

#define LG(...) StartLog::instance().write(__VA_ARGS__)
