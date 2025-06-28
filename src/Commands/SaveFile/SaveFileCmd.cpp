#include "SaveFileCmd.h"
#include "FileData.h"

#include <fstream>
#include <codecvt>
#include <locale>
#include <filesystem>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <map>

namespace ccc
{
    std::string wstringToString(const std::wstring& wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

    std::wstring stringToWstring(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(str);
    }
}

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

    return ccc::stringToWstring(monthNames[localTime->tm_mon + 1]);
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

        if (!std::filesystem::exists(timeStampedPath)) {
            std::filesystem::create_directories(timeStampedPath);
        }

        return timeStampedPath / filePath.filename();
    }

    return basePath / filePath.filename();
}

std::vector<uint8_t> SaveFileCmd::execute(const std::vector<uint8_t>& data) {
    std::wstring filename;
    std::vector<uint8_t> fileContent;

    try {
        FileData fd = deserializeFromBytes(data);

        filename = ccc::stringToWstring(fd.getFilename());
        fileContent = fd.getData();
    } catch (...) {
        return {0};
    }

    std::filesystem::path basePath = getBasePath();
    if (!std::filesystem::exists(basePath)) {
        std::filesystem::create_directories(basePath);
    }

    std::filesystem::path fullPath = createTimestampedPath(basePath, filename);

    std::wstring uniqueFilename = getUniqueFilename(fullPath.parent_path(), fullPath.filename().wstring(), fileContent);
    fullPath = fullPath.parent_path() / uniqueFilename;

    std::ofstream file(fullPath, std::ios::binary | std::ios::out | std::ios::trunc);

    if (!file.is_open()) {
        return {0};
    }

    file.write(reinterpret_cast<const char*>(fileContent.data()), fileContent.size());

    if (!file.good()) {
        return {0};
    }

    file.close();
    return {1};
}
