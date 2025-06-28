//
// Created by Sg on 29.09.2024.
//

#include "TelegaWay.h"
#include "SQLite/mySql.h"
#include <chrono>
#include <ctime>

// Вспомогательная функция, чтобы получить текущий год в виде строки
static std::string getCurrentYear() {
    using clock = std::chrono::system_clock;
    std::time_t t = std::chrono::system_clock::to_time_t(clock::now());
    std::tm* tmPtr = std::localtime(&t);
    int year = 1900 + tmPtr->tm_year;
    return std::to_string(year);
}


// Функция для получения текущей даты в формате dd-mm-yyyy
std::string getCurrentDate() {
    std::time_t now = std::time(nullptr);
    std::tm* localTm = std::localtime(&now);
    char buffer[11]; // "dd-mm-yyyy" + '\0'
    std::strftime(buffer, sizeof(buffer), "%d.%m.%Y", localTm);
    return std::string(buffer);
}

// Функция для получения текущего времени в формате HH:MM:SS
std::string getCurrentTime() {
    std::time_t now = std::time(nullptr);
    std::tm* localTm = std::localtime(&now);
    char buffer[9]; // "HH:MM:SS" + '\0'
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", localTm);
    return std::string(buffer);
}


[[maybe_unused]] TelegaWay::TelegaWay(const std::string& num, Telega::TYPE _type) {


    std::string type_str = _type == Telega::TYPE::VHOD ? "1" : "2";
    auto condition = "where type = " + type_str + " and number = " + num;

    SQL_INST.excSql(base_way_dir,
                    "select * from way " +  condition);

    if (!SQL_EMPTY)
        std::copy(SQL_INST.begin(), SQL_INST.end(), std::back_inserter(result_way));

    const std::string currentYear = getCurrentYear();


    condition = "where type = " + type_str + " and number = " + num + " and print = 0";
    SQL_INST.excSql(base_f12_dir,
                    "select * from tab " +  condition);
    if (SQL_EMPTY)
        return;

    std::map<std::string, std::string> empty_map;

    empty_map["number"] = num;
    empty_map["ddate"]   = getCurrentDate();
    empty_map["ttime"]   = getCurrentTime();
    empty_map["kuda"]   = "L-k:" + SQL_FRONT_VALUE(GdeSht);
    empty_map["type"]   = type_str;
    empty_map["ind"]    = "0";
    empty_map["str"]    = "-";
    empty_map["pos"]    = "-";

    result_way.push_back(empty_map);

}
