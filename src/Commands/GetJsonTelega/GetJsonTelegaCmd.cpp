//
// Created by Sg on 23.05.2024.
//

#include "GetJsonTelegaCmd.h"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
    nlohmann::json telegaToJson(const Telega& value)
    {
        return nlohmann::json{
            {"num", value.num},
            {"type", value.type},
            {"from_to", value.from_to},
            {"isp", value.isp},
            {"podp_num", value.podp_num},
            {"date", value.date},
            {"date_podp", value.date_podp},
            {"dir", value.dir},
            {"rel", value.rel},
            {"tel_num", value.tel_num},
            {"kr", value.kr},
            {"pril_name", value.pril_name},
            {"pril_count", value.pril_count},
            {"blank", value.blank},
            {"gde_sht", value.gde_sht},
            {"last_mesto", value.last_mesto},
            {"deleted", value.deleted},
        };
    }

    command_execution::CommandResult jsonPayloadFromTelegi(
        const std::vector<Telega>& telegi)
    {
        nlohmann::json jsonTelegi = nlohmann::json::array();
        for (const auto& telega : telegi)
            jsonTelegi.push_back(telegaToJson(telega));
        const std::string jsonString = jsonTelegi.dump();
        return command_execution::CommandResult::success(
            {jsonString.begin(), jsonString.end()});
    }
}

command_execution::CommandResult GetJsonTelegaCmd::getSqlJsonTelegaResult(
    const std::vector<uint8_t>& data,
    Telega::TYPE type)
{
    if (!Telega::isSourceConfigured(type)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DataSourceDisabled,
            Telega::disabledDiagnostic(type));
    }

    try {
        Telega::ensureBasesLoaded(type);
    }
    catch (const Telega::SourceError& error) {
        return mapAutoPadSourceError(error);
    }

    const std::string request(data.begin(), data.end());
    std::vector<Telega> telegi;
    try {
        auto res = Telega::findBase(request, type);
        telegi.reserve(res.size());

        for (const auto& row : res) {
            Telega telega(row, type);

            std::error_code fileError;
            telega.deleted = telega.dir.empty() ||
                !std::filesystem::exists(
                    std::filesystem::u8path(telega.dir), fileError);

            telegi.push_back(std::move(telega));
        }
    }
    catch (const Telega::SourceError& error) {
        return mapAutoPadSourceError(error);
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DataSourceUnavailable,
            std::string(Telega::sourceLabel(type)) +
                " data source is unavailable: " + error.what());
    }

    return jsonPayloadFromTelegi(telegi);
}

std::vector<uint8_t> GetJsonTelegaCmd::getSqlJsonTelega(
    const std::vector<uint8_t>& data,
    Telega::TYPE type)
{
    auto result = getSqlJsonTelegaResult(data, type);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

std::vector<uint8_t> GetJsonTelegaVhCmd::execute(const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetJsonTelegaVhCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    return getSqlJsonTelegaResult(data, Telega::TYPE::VHOD);
}

std::vector<uint8_t> GetJsonTelegaIshCmd::execute(const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetJsonTelegaIshCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    return getSqlJsonTelegaResult(data, Telega::TYPE::ISHOD);
}
