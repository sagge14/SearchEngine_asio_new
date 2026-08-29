//
// Created by Sg on 29.09.2024.
//

#include "GetTelegaWayCmd.h"
#include "nlohmann/json.hpp"
#include "SQLite/mySQLite.h"
#include "TelegaWay.h"

#include <utility>

namespace
{
    command_execution::CommandResult mapTelegaWayError(const SQLiteOpenError& error)
    {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DatabaseOpenFailed,
            error.what());
    }

    command_execution::CommandResult mapTelegaWayError(const SQLiteQueryError& error)
    {
        return command_execution::CommandResult::failure(
            error.isSchemaFailure()
                ? command_execution::ErrorCode::DatabaseSchemaFailed
                : command_execution::ErrorCode::DatabaseQueryFailed,
            error.what());
    }
}

std::vector<uint8_t> GetTelegaWayCmd:: GetTelegaWay(const std::vector<uint8_t> &_data, Telega::TYPE _type) {

    namespace nh = nlohmann;
    std::string request(_data.begin(), _data.end());
    nh::json jsonTelegi;
    jsonTelegi = TelegaWay{request,_type}.result_way;
    std::string jsonString = jsonTelegi.dump();

    return std::vector<uint8_t>(jsonString.begin(), jsonString.end());
}

command_execution::CommandResult GetTelegaWayCmd::GetTelegaWayResult(
    const std::vector<uint8_t>& data,
    Telega::TYPE type)
{
    try {
        return command_execution::CommandResult::success(GetTelegaWay(data, type));
    }
    catch (const SQLiteOpenError& error) {
        return mapTelegaWayError(error);
    }
    catch (const SQLiteQueryError& error) {
        return mapTelegaWayError(error);
    }
    catch (const std::out_of_range& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DatabaseSchemaFailed,
            std::string("F12 schema is missing a required column: ") +
                error.what() + " (path=" + TelegaWay::base_f12_dir + ")");
    }
}

std::vector<uint8_t> GetTelegaWayVhCmd::execute(const std::vector<uint8_t> &data) {
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetTelegaWayVhCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    return GetTelegaWayResult(data, Telega::TYPE::VHOD);
}

std::vector<uint8_t> GetTelegaWayIshCmd::execute(const std::vector<uint8_t> &data) {
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetTelegaWayIshCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    return GetTelegaWayResult(data, Telega::TYPE::ISHOD);
}
