//
// Created by Sg on 25.08.2025.
//
#include "nlohmann/json.hpp"
#include "GetIshTelegaPdtvCommand.h"
#include "Commands/GetJsonTelega/GetJsonTelegaCmd.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "SQLite/SQLiteConnectionManager.h"
#include <string>

std::vector<uint8_t> GetIshTelegaPdtvCommand::execute(const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetIshTelegaPdtvCommand::executeResult(
    const std::vector<uint8_t>& data)
{
    namespace nh = nlohmann;

    if (!Telega::isSourceConfigured(Telega::TYPE::ISHOD)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DataSourceDisabled,
            Telega::disabledDiagnostic(Telega::TYPE::ISHOD));
    }

    try {
        Telega::ensureBasesLoaded(Telega::TYPE::ISHOD);
    }
    catch (const Telega::SourceError& error) {
        return mapAutoPadSourceError(error);
    }

    const std::string request(data.begin(), data.end());
    const auto sql_qry =
        "select `index`, pdtv, allpdtv1 from archive where `index` = '" +
        request + "'";

    std::list<std::map<std::string, std::string>> result{};
    try {
        for (const auto& base_name : Telega::b_prd) {
            auto db = SQLiteConnectionManager::instance().getConnection(base_name);
            db->execSql(sql_qry);

            if (!db->empty()) {
                for (const auto& row : *db) {
                    result.push_back(row);
                }
            }
        }
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DataSourceUnavailable,
            std::string("PRD data source is unavailable: ") + error.what());
    }

    nh::json jsonTelegi = result;
    const std::string jsonString = jsonTelegi.dump();
    return command_execution::CommandResult::success(
        {jsonString.begin(), jsonString.end()});
}
