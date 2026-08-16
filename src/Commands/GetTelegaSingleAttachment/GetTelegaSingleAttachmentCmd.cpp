//
// Created by Sg on 08.09.2025.
//

#include "GetTelegaSingleAttachmentCmd.h"

#include "Commands/GetJsonTelega/AutoPadSource.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "MyUtils/Encoding.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

std::vector<uint8_t> GetTelegaSingleAttachmentCmd::execute(
    const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetTelegaSingleAttachmentCmd::executeResult(
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
        !jsonRequest["type"].is_number_integer() ||
        !jsonRequest.contains("file_name") ||
        !jsonRequest["file_name"].is_string()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "single attachment request requires integer id/type and file_name");
    }

    std::int64_t wireId{};
    std::int64_t wireType{};
    std::string fileName;
    try {
        wireId = jsonRequest["id"].get<std::int64_t>();
        wireType = jsonRequest["type"].get<std::int64_t>();
        fileName = jsonRequest["file_name"].get<std::string>();
    }
    catch (const nh::json::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            error.what());
    }
    if (wireId < 0 || wireId > (std::numeric_limits<int>::max)() ||
        (wireType != static_cast<int>(Telega::TYPE::VHOD) &&
         wireType != static_cast<int>(Telega::TYPE::ISHOD)) ||
        fileName.empty()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "single attachment id, type, or file_name is invalid");
    }

    const fs::path requestedPath(fileName);
    if (requestedPath.is_absolute() ||
        requestedPath.has_root_path() ||
        requestedPath.has_parent_path() ||
        requestedPath.filename() != requestedPath) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "file_name must contain only an attachment basename");
    }

    const int telegramId = static_cast<int>(wireId);
    const auto telegramType = static_cast<Telega::TYPE>(wireType);

    // Empty base dir: source intentionally disabled — no attachment data here.
    if (!Telega::isSourceConfigured(telegramType)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::AttachmentNotFound,
            "telegram has no attachments");
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

    if (attachmentNames.empty()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::AttachmentNotFound,
            "telegram has no attachments");
    }

    bool advertised = false;
    if (telegramType == Telega::TYPE::VHOD) {
        std::stringstream names(attachmentNames);
        std::string name;
        while (std::getline(names, name, ';')) {
            if (name == fileName) {
                advertised = true;
                break;
            }
        }
    }
    else {
        advertised = fileName == std::to_string(telegramId) + ".zip";
    }

    if (!advertised) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::AttachmentNotFound,
            "requested file is not advertised by the telegram");
    }

    fs::path fullPath;
    try {
        const fs::path utf8Path = fs::path(attachmentDirectory) / fileName;
        fullPath = fs::path{encoding::utf8_to_wstring(utf8Path.string())};
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DatabaseSchemaFailed,
            error.what());
    }

    std::error_code metadataError;
    const auto status = fs::status(fullPath, metadataError);
    if (metadataError) {
        if (metadataError == std::errc::no_such_file_or_directory) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::AttachmentNotFound,
                fileName);
        }
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileMetadataFailed,
            metadataError.message());
    }
    if (!fs::exists(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::AttachmentNotFound,
            fileName);
    }
    if (!fs::is_regular_file(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            "attachment path is not a regular file");
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            fileName);
    }

    std::vector<uint8_t> fileContent(
        std::istreambuf_iterator<char>{file},
        std::istreambuf_iterator<char>{});
    if (file.bad()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileReadFailed,
            fileName);
    }

    return command_execution::CommandResult::success(std::move(fileContent));
}
