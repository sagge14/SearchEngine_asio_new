//
// Created by Sg on 31.08.2025.
//

#include "GetTelegaAttachments.h"

#include "Commands/GetJsonTelega/AutoPadSource.h"
#include "MyUtils/Encoding.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

std::vector<uint8_t> GetTelegaAttachmentsCmd::execute(
    const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetTelegaAttachmentsCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;

    nh::json jsonRequest;
    try {
        jsonRequest = nh::json::parse(std::string(data.begin(), data.end()));
    }
    catch (const nh::json::parse_error& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidJson,
            error.what());
    }

    if (!jsonRequest.is_object() ||
        !jsonRequest.contains("id") ||
        !jsonRequest["id"].is_number_integer() ||
        !jsonRequest.contains("type") ||
        !jsonRequest["type"].is_number_integer()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "attachment list request requires integer id and type");
    }

    std::int64_t wireId{};
    std::int64_t wireType{};
    try {
        wireId = jsonRequest["id"].get<std::int64_t>();
        wireType = jsonRequest["type"].get<std::int64_t>();
    }
    catch (const nh::json::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            error.what());
    }
    if (wireId < 0 || wireId > (std::numeric_limits<int>::max)() ||
        (wireType != static_cast<int>(Telega::TYPE::VHOD) &&
         wireType != static_cast<int>(Telega::TYPE::ISHOD))) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "attachment list id or type is outside the supported range");
    }

    const int telegramId = static_cast<int>(wireId);
    const auto telegramType = static_cast<Telega::TYPE>(wireType);

    // Empty base dir: source intentionally disabled — normal empty attachment list.
    if (!Telega::isSourceConfigured(telegramType)) {
        const nh::json empty = nh::json::array();
        const std::string serialized = empty.dump();
        return command_execution::CommandResult::success(
            {serialized.begin(), serialized.end()});
    }
    try {
        Telega::ensureBasesLoaded(telegramType);
    }
    catch (const Telega::SourceError& error) {
        return mapAutoPadSourceError(error);
    }

    const auto* bases = telegramType == Telega::TYPE::VHOD
        ? &Telega::b_prm
        : &Telega::b_prd;

    const std::string sql =
        "SELECT PrilName, DirectTo FROM archive WHERE `index` = " +
        std::to_string(telegramId);

    std::string attachmentNames;
    std::string attachmentDirectory;
    for (const auto& baseName : *bases) {
        std::shared_ptr<mySQLite> database;
        try {
            database = SQLiteConnectionManager::instance().getConnection(baseName);
        }
        catch (const std::exception& error) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DatabaseOpenFailed,
                error.what());
        }

        mySQLite::RowList rows;
        try {
            rows = database->queryRows(sql);
        }
        catch (const std::exception& error) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DatabaseQueryFailed,
                error.what());
        }

        if (rows.empty())
            continue;

        const auto& row = rows.front();
        const auto names = row.find("PrilName");
        const auto directory = row.find("DirectTo");
        if (names == row.end() || directory == row.end()) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DatabaseSchemaFailed,
                "archive row has no PrilName or DirectTo column");
        }
        attachmentNames = names->second;
        attachmentDirectory = directory->second;
        break;
    }

    nh::json response = nh::json::array();
    if (attachmentNames.empty()) {
        const std::string serialized = response.dump();
        return command_execution::CommandResult::success(
            {serialized.begin(), serialized.end()});
    }

    auto appendItem = [&](const std::string& name)
        -> std::optional<command_execution::CommandResult>
    {
        fs::path fullPath;
        try {
            const fs::path utf8Path = fs::path(attachmentDirectory) / name;
            fullPath = fs::path{encoding::utf8_to_wstring(utf8Path.string())};
        }
        catch (const std::exception& error) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DatabaseSchemaFailed,
                error.what());
        }

        std::error_code metadataError;
        const bool exists = fs::exists(fullPath, metadataError);
        if (metadataError) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::FileMetadataFailed,
                metadataError.message());
        }

        std::uintmax_t size = 0;
        if (exists) {
            size = fs::file_size(fullPath, metadataError);
            if (metadataError) {
                return command_execution::CommandResult::failure(
                    command_execution::ErrorCode::FileMetadataFailed,
                    metadataError.message());
            }
        }

        nh::json item;
        item["name"] = fs::path(name).filename().string();
        item["exists"] = exists;
        item["size"] = size;
        response.push_back(std::move(item));
        return std::nullopt;
    };

    if (telegramType == Telega::TYPE::VHOD) {
        std::stringstream names(attachmentNames);
        std::string name;
        while (std::getline(names, name, ';')) {
            if (name.empty())
                continue;
            if (auto error = appendItem(name))
                return std::move(*error);
        }
    }
    else {
        if (auto error = appendItem(std::to_string(telegramId) + ".zip"))
            return std::move(*error);
    }

    try {
        const std::string serialized = response.dump();
        return command_execution::CommandResult::success(
            {serialized.begin(), serialized.end()});
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::SerializationFailed,
            error.what());
    }
}
