//
// Created by user on 06.08.2023.
//

#include "Resolution.h"
#include <fstream>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include "SQLite/mySql.h"
#include <codecvt>
#include <locale>

namespace conv
{
    using namespace std;
    using convert_t = std::codecvt_utf8<wchar_t>;
    wstring_convert<convert_t, wchar_t> strconverter;

    string to_string(std::wstring wstr)                                 //Конструкция перевода в String из WString
    {
        return strconverter.to_bytes(wstr);
    }

    wstring to_wstring(std::string str)                                 //Конструкция перевода в WString из String
    {
        return strconverter.from_bytes(str);
    }
}



std::string Resolution::getLastN_RES() {

    mySql::excSql("SELECT N_RES FROM RESOLUTIONS ORDER BY N_RES DESC LIMIT 1");
    return SQL_FRONT_VALUE(N_RES);
}

int Resolution::addResolution() {

    #define STR_ENUM(e) std::to_string((size_t)(e))

    char date[20];
    char time[20];
    time_t now = std::time(nullptr);
    strftime( time, sizeof(date),"%H:%M:%S", localtime(&now));
    strftime( date, sizeof(date),"%d.%m.%Y", localtime(&now));


    mySql::excSql("D:\\BASES\\ARCHIVE.db3", "SELECT DirectTo, FileName FROM ARCHIVE WHERE `Index` = " + n_sht );
    dir_sht =  SQL_FRONT_VALUE(DirectTo) + SQL_FRONT_VALUE(FileName);

    auto sql = ("INSERT INTO RESOLUTIONS (N_SHT, DDATE, TTIME, ID_R, RES_DIR, CHK, SHT_DIR, DEAD_LINE) VALUES ('" + n_sht + "','" + date + "','" + time + "','" + id_r + "','" + file_name + "','" + chk + "','" + dir_sht + "','" + dead_line + "')");
    mySql::excSql(sql);

    auto last_res_n = getLastN_RES();

    file_name = "RESOLUTIONS\\res_" + last_res_n + ".txt";

    sql =  "UPDATE RESOLUTIONS set RES_DIR = '" + file_name + "' where N_RES = " + last_res_n;
    mySql::excSql(sql);


    for(const auto& i:isp)
    {
        auto sql2 =  ("INSERT INTO EXECUTORS (N_RES, ID, TYPE, N_SHT, STATUS, DEAD_LINE) VALUES ('" + last_res_n + "','" + i + "','" + STR_ENUM(EXC::ISP) + "','" + n_sht + "', '0', '" + dead_line + "')");
        mySql::excSql(sql2);
    }
    for(const auto& i:so_isp)
    {
        auto sql2 =  ("INSERT INTO EXECUTORS (N_RES, ID, TYPE, N_SHT, STATUS, DEAD_LINE) VALUES ('" + last_res_n + "','" + i + "','" + STR_ENUM(EXC::SO_ISP) + "','" + n_sht + "', '0', NULL");
        mySql::excSql(sql2);
    }

    return 0;
}

int Resolution::saveResolutions() {

    std::locale::global(std::locale(""));
    std::ofstream resFile;
    resFile.open(file_name);
    resFile << text;
    resFile.close();

    return 0;
}


