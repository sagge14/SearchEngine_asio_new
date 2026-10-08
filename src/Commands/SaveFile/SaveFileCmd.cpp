#include "SaveFileCmd.h"
#include "FileData.h"

#include "MyUtils/Encoding.h"
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <map>

std::wstring SaveFileCmd::getUniqueFilename(const std::filesystem::path& directory, const std::wstring& filename, const std::vector<uint8_t>& fileContent) {
    std::filesystem::path filePath = directory / filename;

    if (std::filesystem::exists(filePath)) {
        std::uintmax_t existingFileSize = std::filesystem::file_size(filePath);
        if (existingFileSize == fileContent.size()) {
            return filename;
        }
    }

    std::wstring extension = filePath.extension().wstring();
    std::wstring baseName = filePath.stem().wstring();

    int index = 1;
    wchar_t letter = L'a';

    while (std::filesystem::exists(filePath)) {
        std::wstringstream newFileName;

        if (index <= 1000) {
            newFileName << baseName << L"(" << index << L")" << extension;
        } else {
            newFileName << baseName << L"(" << index << letter << L")" << extension;
            if (letter < L'z') {
                letter++;
            } else {
                letter = L'a';
                index++;
            }
        }

        filePath = directory / newFileName.str();
        index++;
    }

    return filePath.filename().wstring();
}

std::wstring SaveFileCmd::getCurrentDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm* localTime = std::localtime(&currentTime);

    std::wstringstream dateStream;
    dateStream << std::put_time(localTime, L"%d.%m.%y");

    return dateStream.str();
}

std::wstring SaveFileCmd::getCurrentMonthInRussianUpperCase() {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm* localTime = std::localtime(&currentTime);

    std::map<int, std::string> monthNames = {
            {1, "ЯНВАРЬ"}, {2, "ФЕВРАЛЬ"}, {3, "МАРТ"}, {4, "АПРЕЛЬ"},
            {5, "МАЙ"}, {6, "ИЮНЬ"}, {7, "ИЮЛЬ"}, {8, "АВГУСТ"},
            {9, "СЕНТЯБРЬ"}, {10, "ОКТЯБРЬ"}, {11, "НОЯБРЬ"}, {12, "ДЕКАБРЬ"}
    };

    return encoding::utf8_to_wstring(monthNames[localTime->tm_mon + 1]);
}

std::filesystem::path createTimestampedPath(const std::filesystem::path& basePath, const std::wstring& filename) {
    std::filesystem::path filePath = filename;

    if (filename.find(L'\\') != std::wstring::npos || filename.find(L'/') != std::wstring::npos) {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime;
#ifdef _WIN32
        localtime_s(&localTime, &nowTime);
#else
        localtime_r(&nowTime, &localTime);
#endif

        std::wostringstream timeStream;
        timeStream << std::put_time(&localTime, L"%H-%M");

        std::filesystem::path relativePath = filePath.parent_path();
        std::filesystem::path timeStampedPath = basePath / relativePath;
        timeStampedPath += L"_" + timeStream.str();

        return timeStampedPath / filePath.filename();
    }

    return basePath / filePath.filename();
}

std::vector<uint8_t> SaveFileCmd::execute(const std::vector<uint8_t>& data) {
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{0};
}

command_execution::CommandResult SaveFileCmd::executeResult(
    const std::vector<uint8_t>& data) {
    FileData fileData;

    try {
        fileData = deserializeFromBytes(data);
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidBinaryPayload,
            error.what());
    }
    catch (...) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidBinaryPayload,
            "unknown FileData deserialization error");
    }

    std::wstring filename;
    try {
        filename = encoding::utf8_to_wstring(fileData.getFilename());
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            error.what());
    }
    if (filename.empty()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            "FileData filename is empty");
    }

    std::filesystem::path basePath;
    try {
        basePath = getBasePath();
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::ConfigurationError,
            error.what());
    }
    if (basePath.empty()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::ConfigurationError,
            "save directory is empty");
    }

    std::error_code filesystemError;
    const bool basePathExists = std::filesystem::exists(basePath, filesystemError);
    if (filesystemError) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileMetadataFailed,
            filesystemError.message());
    }
    if (basePathExists) {
        const bool isDirectory =
            std::filesystem::is_directory(basePath, filesystemError);
        if (filesystemError) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::FileMetadataFailed,
                filesystemError.message());
        }
        if (!isDirectory) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::ConfigurationError,
                "save directory points to a non-directory entry");
        }
    }
    else {
        std::filesystem::create_directories(basePath, filesystemError);
        if (filesystemError) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DirectoryCreateFailed,
                filesystemError.message());
        }
    }

    std::filesystem::path fullPath;
    try {
        fullPath = createTimestampedPath(basePath, filename);
    }
    catch (const std::exception& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::InvalidRequest,
            error.what());
    }

    const auto destinationDirectory = fullPath.parent_path();
    std::filesystem::create_directories(destinationDirectory, filesystemError);
    if (filesystemError) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::DirectoryCreateFailed,
            filesystemError.message());
    }

    try {
        const std::wstring uniqueFilename = getUniqueFilename(
            destinationDirectory,
            fullPath.filename().wstring(),
            fileData.getData());
        fullPath = destinationDirectory / uniqueFilename;
    }
    catch (const std::filesystem::filesystem_error& error) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileMetadataFailed,
            error.what());
    }

    std::ofstream file(fullPath, std::ios::binary | std::ios::out | std::ios::trunc);

    if (!file.is_open()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileOpenFailed,
            "failed to open destination file");
    }

    const auto& fileContent = fileData.getData();
    if (!fileContent.empty()) {
        file.write(
            reinterpret_cast<const char*>(fileContent.data()),
            static_cast<std::streamsize>(fileContent.size()));
    }

    if (!file.good()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileWriteFailed,
            "failed to write destination file");
    }

    file.close();
    if (file.fail()) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::FileWriteFailed,
            "failed to close destination file");
    }

    return command_execution::CommandResult::success({1});
}
