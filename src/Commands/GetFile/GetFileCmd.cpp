//
// Created by Sg on 23.05.2024.
//

#include "GetFileCmd.h"
#include "MyUtils/Utf8Path.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

std::vector<uint8_t> GetFileCmd::execute(const std::vector<uint8_t>& data) {
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetFileCmd::executeResult(
    const std::vector<uint8_t>& data) {
    return action(data);
}

std::vector<uint8_t> GetFileCmd::downloadFileByPath(const std::string& data) {
    auto result = downloadFileResultByPath(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

std::vector<uint8_t> GetFileCmd::downloadFileByPath(const std::vector<uint8_t> &data) {
    auto result = downloadFileResultByPath(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetFileCmd::downloadFileResultByPath(
    const std::string& data) {
    return downloadFileResultByPath(
        std::vector<uint8_t>{data.begin(), data.end()});
}

command_execution::CommandResult GetFileCmd::downloadFileResultByPath(
    const std::vector<uint8_t>& data) {
    if (data.empty() ||
        std::find(data.begin(), data.end(), static_cast<uint8_t>(0)) != data.end()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "file path is empty or contains a NUL byte");
    }

    const std::string filename(data.begin(), data.end());
    const std::filesystem::path filePath(filename);

    std::error_code metadataError;
    const auto status = std::filesystem::status(filePath, metadataError);
    if (metadataError) {
        if (metadataError == std::errc::no_such_file_or_directory) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::FileNotFound,
                filename);
        }
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileMetadataFailed,
            metadataError.message());
    }
    if (!std::filesystem::exists(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileNotFound,
            filename);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            "path is not a regular file: " + filename);
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            filename);
    }

    std::vector<uint8_t> fileContent(
        std::istreambuf_iterator<char>{file},
        std::istreambuf_iterator<char>{});
    if (file.bad()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileReadFailed,
            filename);
    }

    return command_execution::CommandResult::success(std::move(fileContent));
}

command_execution::CommandResult GetFileCmd::downloadFileResultByPath(
    const std::filesystem::path& filePath)
{
    const std::string filename = encoding::path_to_utf8(filePath);
    if (filePath.empty() || filename.empty()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "file path is empty or contains a NUL byte");
    }

    std::error_code metadataError;
    const auto status = std::filesystem::status(filePath, metadataError);
    if (metadataError) {
        if (metadataError == std::errc::no_such_file_or_directory) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::FileNotFound,
                filename);
        }
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileMetadataFailed,
            metadataError.message());
    }
    if (!std::filesystem::exists(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileNotFound,
            filename);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            "path is not a regular file: " + filename);
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            filename);
    }

    std::vector<uint8_t> fileContent(
        std::istreambuf_iterator<char>{file},
        std::istreambuf_iterator<char>{});
    if (file.bad()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileReadFailed,
            filename);
    }

    return command_execution::CommandResult::success(std::move(fileContent));
}

command_execution::CommandResult GetFileCmd::rejectRawBinFileDownload(
    const std::vector<uint8_t>& requestData)
{
    (void)requestData;
    return command_execution::CommandResult::failure(
        command_execution::ErrorCode::InvalidCommand,
        "GETBINFILE raw-path download is disabled");
}

