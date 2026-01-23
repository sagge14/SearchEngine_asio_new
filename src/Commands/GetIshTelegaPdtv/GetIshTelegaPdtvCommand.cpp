//
// Created by Sg on 25.08.2025.
//
#include "nlohmann/json.hpp"
#include "GetIshTelegaPdtvCommand.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "SQLite/SQLiteConnectionManager.h"
#include <string>


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

            auto db = SQLiteConnectionManager::instance().getConnection(base_name);
            db->execSql(sql_qry);

            if (!db->empty()) {
                for (const auto& row : *db) {
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
    return {jsonString.begin(), jsonString.end()};
}