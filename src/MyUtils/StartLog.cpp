#include "StartLog.h"

StartLog& StartLog::instance()
{
    static StartLog s("startup.log");
    return s;
}

StartLog::StartLog(const char* file)
        : stream_(file, std::ios::app)
{
    write("=== ===  Запуск программы  === ===");
}

std::string StartLog::timestamp()
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
