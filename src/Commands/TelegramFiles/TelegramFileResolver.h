#pragma once

#include "Commands/CommandResult.h"
#include "Commands/GetJsonTelega/Telega.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct TelegramArchiveRecord
{
    std::string prilName;
    std::string directTo;
    std::string fileName;
};

struct TelegramArchiveLookupResult
{
    std::optional<TelegramArchiveRecord> record;
    std::optional<command_execution::ErrorCode> error;
    std::string diagnostic;

    [[nodiscard]] bool failed() const noexcept
    {
        return error.has_value();
    }
};

[[nodiscard]] TelegramArchiveLookupResult lookupTelegramArchive(
    int telegramId,
    Telega::TYPE type);

struct ResolvedTelegramFile
{
    std::shared_ptr<std::ifstream> stream;
    std::uint64_t size{0};
    std::filesystem::path path;
};

struct ResolveTelegramFileResult
{
    std::optional<ResolvedTelegramFile> file;
    std::optional<command_execution::ErrorCode> error;
    std::string diagnostic;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return file.has_value();
    }

    [[nodiscard]] bool failed() const noexcept
    {
        return !file.has_value();
    }
};

class TelegramFileResolver
{
public:
    [[nodiscard]] static ResolveTelegramFileResult resolveAttachment(
        const std::vector<std::uint8_t>& request);

    [[nodiscard]] static ResolveTelegramFileResult resolveText(
        const std::vector<std::uint8_t>& request);
};
