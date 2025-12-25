//
// Created by Sg on 25.08.2025.
//
#include "nlohmann/json.hpp"
#include "SQLite/mySql.h"
#include <string>
#include "GetIshTelegaPdtvCommand.h"
#include "Commands/GetJsonTelega/Telega.h"




std::vector<uint8_t> GetIshTelegaPdtvCommand::execute(const std::vector<uint8_t>& _data)
{
    namespace nh = nlohmann;
    std::string request(_data.begin(), _data.end());
    nh::json jsonTelegi;

    const decltype(Telega::getBases(Telega::TYPE{})) *base;
    std::list<std::map<std::string,std::string>> result{};


    auto sql_qry = "select `index`, pdtv, allpdtv1 from archive where `index` = '" + request + "'";

    base = &Telega::b_prd;

    try {
        for (const auto &base_name: *base) {

            SQL_INST.excSql(base_name, sql_qry);

            if (!SQL_EMPTY) {
                for (const auto& row : SQL_INST) {
                    result.push_back(row);
                }
            }
        }
    }
    catch(...)
    {std::cout << "sql error" << std::endl;}

    jsonTelegi = result;

    std::string jsonString = jsonTelegi.dump();
    std::cout << jsonString << std::endl;
    return std::vector<uint8_t>(jsonString.begin(), jsonString.end());
}