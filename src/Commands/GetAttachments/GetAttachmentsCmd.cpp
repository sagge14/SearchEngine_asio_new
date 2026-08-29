//
// Created by Sg on 29.10.2024.
//

#include "GetAttachmentsCmd.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include "PrefixMap.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
    PrefixMap loadAttachmentConfiguration(
        const std::filesystem::path& configPath)
    {
        std::ifstream file(configPath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error(
                "cannot open attachment configuration: " +
                configPath.string());
        }

        const auto jsonFile = nlohmann::json::parse(file);
        if (!jsonFile.is_object() ||
            !jsonFile.contains("prefix") ||
            !jsonFile["prefix"].is_string() ||
            !jsonFile.contains("map") ||
            !jsonFile["map"].is_object()) {
            throw std::runtime_error(
                "attachment configuration requires string prefix and object map");
        }

        PrefixMap result;
        result.prefix = encoding::utf8_to_wstring(
            jsonFile["prefix"].get<std::string>());
        for (const auto& [key, value] : jsonFile["map"].items()) {
            if (!value.is_string()) {
                throw std::runtime_error(
                    "attachment configuration map values must be strings");
            }
            result.map_.emplace(
                encoding::utf8_to_wstring(key),
                encoding::utf8_to_wstring(value.get<std::string>()));
        }
        return result;
    }
}

GetAttachmentsCmd::GetAttachmentsCmd(std::filesystem::path configPath)
    : configPath_(std::move(configPath))
{
}

void GetAttachmentsCmd::deleteDirectory(const std::filesystem::path& dirPath) {
    try {
        if (std::filesystem::exists(dirPath)) {
            std::filesystem::remove_all(dirPath);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LogFile::getErrors().write(
            "GET_ATTACHMENTS cleanup failed: " +
            dirPath.string() + " (" + e.what() + ")");
    }
}

std::vector<uint8_t> GetAttachmentsCmd::execute(
    const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetAttachmentsCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    namespace fs = std::filesystem;
    using command_execution::CommandResult;
    using command_execution::ErrorCode;

    if (data.empty() ||
        std::find(data.begin(), data.end(), static_cast<uint8_t>(0)) !=
            data.end()) {
        return CommandResult::failure(
            ErrorCode::InvalidRequest,
            "operator name is empty or contains a null byte");
    }

    PrefixMap configuration;
    try {
        configuration = loadAttachmentConfiguration(configPath_);
    }
    catch (const std::exception& error) {
        return CommandResult::failure(
            ErrorCode::ConfigurationError,
            error.what());
    }
    catch (...) {
        return CommandResult::failure(
            ErrorCode::ConfigurationError,
            "unknown attachment configuration error");
    }

    const std::wstring userName(data.begin(), data.end());
    const auto operatorPath = configuration.map_.find(userName);
    if (operatorPath == configuration.map_.end()) {
        return CommandResult::failure(
            ErrorCode::OperatorNotRegistered,
            "operator is not registered in attachment configuration");
    }

    fs::path attachmentDirectory;
    try {
        attachmentDirectory = fs::path(
            configuration.prefix + operatorPath->second);
    }
    catch (const std::exception& error) {
        return CommandResult::failure(
            ErrorCode::ConfigurationError,
            error.what());
    }
    if (attachmentDirectory.empty()) {
        return CommandResult::failure(
            ErrorCode::ConfigurationError,
            "configured attachment directory is empty");
    }

    std::error_code metadataError;
    const auto directoryStatus = fs::status(attachmentDirectory, metadataError);
    if (metadataError) {
        if (metadataError == std::errc::no_such_file_or_directory) {
            return CommandResult::failure(
                ErrorCode::AttachmentNotFound,
                attachmentDirectory.string());
        }
        return CommandResult::failure(
            ErrorCode::FileMetadataFailed,
            metadataError.message());
    }
    if (!fs::exists(directoryStatus)) {
        return CommandResult::failure(
            ErrorCode::AttachmentNotFound,
            attachmentDirectory.string());
    }
    if (!fs::is_directory(directoryStatus)) {
        return CommandResult::failure(
            ErrorCode::ConfigurationError,
            "configured attachment path is not a directory");
    }

    AttachmentPackage message;
    std::error_code iterationError;
    fs::recursive_directory_iterator entry(
        attachmentDirectory,
        fs::directory_options::none,
        iterationError);
    const fs::recursive_directory_iterator end;
    if (iterationError) {
        return CommandResult::failure(
            ErrorCode::FileOpenFailed,
            iterationError.message());
    }

    while (entry != end) {
        std::error_code typeError;
        const bool isRegularFile = entry->is_regular_file(typeError);
        if (typeError) {
            return CommandResult::failure(
                ErrorCode::FileMetadataFailed,
                typeError.message());
        }

        if (isRegularFile) {
            const fs::path filePath = entry->path();
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                return CommandResult::failure(
                    ErrorCode::FileOpenFailed,
                    filePath.string());
            }

            std::vector<uint8_t> fileContent(
                std::istreambuf_iterator<char>{file},
                std::istreambuf_iterator<char>{});
            if (file.bad()) {
                return CommandResult::failure(
                    ErrorCode::FileReadFailed,
                    filePath.string());
            }

            std::error_code relativeError;
            const auto relativePath = fs::relative(
                filePath,
                attachmentDirectory,
                relativeError);
            if (relativeError) {
                return CommandResult::failure(
                    ErrorCode::FileMetadataFailed,
                    relativeError.message());
            }
            message.attachments[relativePath.string()] = std::move(fileContent);
        }

        entry.increment(iterationError);
        if (iterationError) {
            return CommandResult::failure(
                ErrorCode::FileReadFailed,
                iterationError.message());
        }
    }

    // Legacy buffer semantics: remove the source directory after all files
    // were loaded and before serializing the response.
    deleteDirectory(attachmentDirectory);

    try {
        return CommandResult::success(AttachmentPackage::serializeToBytes(message));
    }
    catch (const std::exception& error) {
        return CommandResult::failure(
            ErrorCode::SerializationFailed,
            error.what());
    }
    catch (...) {
        return CommandResult::failure(
            ErrorCode::SerializationFailed,
            "unknown attachment serialization error");
    }
}
