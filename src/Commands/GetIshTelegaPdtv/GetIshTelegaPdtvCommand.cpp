//
// Created by Sg on 25.08.2025.
//
#include "nlohmann/json.hpp"
#include "GetIshTelegaPdtvCommand.h"
#include "Commands/GetJsonTelega/GetJsonTelegaCmd.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/mySQLite.h"

#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace
{
    using command_execution::ErrorCode;

    [[nodiscard]] std::optional<int> parseCanonicalNonNegativeInt(
        const std::vector<std::uint8_t>& data)
    {
        if (data.empty())
            return std::nullopt;

        for (const auto byte : data) {
            if (byte < static_cast<std::uint8_t>('0') ||
                byte > static_cast<std::uint8_t>('9')) {
                return std::nullopt;
            }
        }

        unsigned long long value = 0;
        const auto* begin = reinterpret_cast<const char*>(data.data());
        const auto* end = begin + data.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return std::nullopt;
        if (value > static_cast<unsigned long long>((std::numeric_limits<int>::max)()))
            return std::nullopt;

        return static_cast<int>(value);
    }

    [[nodiscard]] command_execution::CommandResult mapSqliteOpenError(
        const std::exception& error)
    {
        return command_execution::CommandResult::failure(
            ErrorCode::DatabaseOpenFailed,
            error.what());
    }

    [[nodiscard]] command_execution::CommandResult mapSqliteQueryError(
        const SQLiteQueryError& error)
    {
        return command_execution::CommandResult::failure(
            error.isSchemaFailure()
                ? ErrorCode::DatabaseSchemaFailed
                : ErrorCode::DatabaseQueryFailed,
            error.what());
    }
}

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

    // Empty prd_base_dir: PRD intentionally disabled — normal empty JSON array.
    if (!Telega::isSourceConfigured(Telega::TYPE::ISHOD)) {
        const std::string empty = "[]";
        return command_execution::CommandResult::success(
            {empty.begin(), empty.end()});
    }

    try {
        Telega::ensureBasesLoaded(Telega::TYPE::ISHOD);
    }
    catch (const Telega::SourceError& error) {
        return mapAutoPadSourceError(error);
    }

    const auto telegramId = parseCanonicalNonNegativeInt(data);
    if (!telegramId.has_value()) {
        return command_execution::CommandResult::failure(
            ErrorCode::InvalidRequest,
            "GET_ISH_PDTV requires a canonical non-negative integer id");
    }

    const auto sql_qry =
        "select `index`, pdtv, allpdtv1 from archive where `index` = " +
        std::to_string(*telegramId);

    std::list<std::map<std::string, std::string>> result{};
    for (const auto& base_name : Telega::b_prd) {
        std::shared_ptr<mySQLite> db;
        try {
            db = SQLiteConnectionManager::instance().getReadOnlyConnection(base_name);
        }
        catch (const SQLiteOpenError& error) {
            return mapSqliteOpenError(error);
        }
        catch (const std::exception& error) {
            return mapSqliteOpenError(error);
        }

        mySQLite::RowList rows;
        try {
            rows = db->queryRows(sql_qry);
        }
        catch (const SQLiteQueryError& error) {
            return mapSqliteQueryError(error);
        }
        catch (const std::exception& error) {
            return command_execution::CommandResult::failure(
                ErrorCode::DatabaseQueryFailed,
                error.what());
        }

        for (auto& row : rows)
            result.push_back(std::move(row));
    }

    nh::json jsonTelegi = result;
    const std::string jsonString = jsonTelegi.dump();
    return command_execution::CommandResult::success(
        {jsonString.begin(), jsonString.end()});
}
