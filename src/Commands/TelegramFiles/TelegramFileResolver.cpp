#include "TelegramFileResolver.h"

#include "Commands/GetJsonTelega/AutoPadSource.h"
#include "MyUtils/Utf8Path.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "nlohmann/json.hpp"

#include <limits>
#include <optional>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;
    using command_execution::ErrorCode;

    constexpr const char* kForbiddenFilesystemKeys[] = {
        "path",
        "dir",
        "DirectTo",
        "FileName",
        "remotePath"};

    ResolveTelegramFileResult fail(ErrorCode error, std::string diagnostic)
    {
        ResolveTelegramFileResult result;
        result.error = error;
        result.diagnostic = std::move(diagnostic);
        return result;
    }

    TelegramArchiveLookupResult lookupFail(ErrorCode error, std::string diagnostic)
    {
        TelegramArchiveLookupResult result;
        result.error = error;
        result.diagnostic = std::move(diagnostic);
        return result;
    }

    bool jsonContainsForbiddenFilesystemField(const nh::json& request)
    {
        for (const char* key : kForbiddenFilesystemKeys) {
            if (request.contains(key))
                return true;
        }
        return false;
    }

    bool parseTelegramIdType(
        const nh::json& request,
        int& telegramId,
        Telega::TYPE& telegramType,
        std::string& diagnostic)
    {
        if (!request.is_object() ||
            !request.contains("id") ||
            !request["id"].is_number_integer() ||
            !request.contains("type") ||
            !request["type"].is_number_integer()) {
            diagnostic = "request requires integer id and type";
            return false;
        }

        std::int64_t wireId{};
        std::int64_t wireType{};
        try {
            wireId = request["id"].get<std::int64_t>();
            wireType = request["type"].get<std::int64_t>();
        }
        catch (const nh::json::exception& error) {
            diagnostic = error.what();
            return false;
        }

        if (wireId < 0 || wireId > (std::numeric_limits<int>::max)() ||
            (wireType != static_cast<int>(Telega::TYPE::VHOD) &&
             wireType != static_cast<int>(Telega::TYPE::ISHOD))) {
            diagnostic = "id or type is outside the supported range";
            return false;
        }

        telegramId = static_cast<int>(wireId);
        telegramType = static_cast<Telega::TYPE>(wireType);
        return true;
    }

    std::optional<nh::json> parseRequestObject(
        const std::vector<std::uint8_t>& data,
        ResolveTelegramFileResult& error)
    {
        try {
            return nh::json::parse(std::string(data.begin(), data.end()));
        }
        catch (const nh::json::parse_error& parseError) {
            error = fail(ErrorCode::InvalidJson, parseError.what());
            return std::nullopt;
        }
    }

    bool isValidAttachmentBaseName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        if (name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos ||
            name.find(':') != std::string::npos ||
            name.find('\0') != std::string::npos) {
            return false;
        }

        try {
            const fs::path path = encoding::utf8_to_path(name);
            if (path.empty() ||
                path.is_absolute() ||
                path.has_root_path() ||
                path.has_root_name() ||
                path.has_root_directory() ||
                path.has_parent_path() ||
                path.filename() != path) {
                return false;
            }
        }
        catch (...) {
            return false;
        }

        return true;
    }

    bool isAdvertisedAttachmentName(
        Telega::TYPE type,
        int telegramId,
        const std::string& prilName,
        const std::string& fileName)
    {
        if (type == Telega::TYPE::VHOD) {
            std::stringstream names(prilName);
            std::string name;
            while (std::getline(names, name, ';')) {
                if (name == fileName)
                    return true;
            }
            return false;
        }

        return fileName == std::to_string(telegramId) + ".zip";
    }

    bool dbFileNameEscapesDirectory(const std::string& fileName)
    {
        if (fileName.empty())
            return true;

        try {
            const fs::path namePath = encoding::utf8_to_path(fileName);
            return namePath.empty() ||
                namePath.is_absolute() ||
                namePath.has_root_path() ||
                namePath.has_root_name() ||
                namePath.has_root_directory();
        }
        catch (...) {
            return true;
        }
    }

    bool compositionStaysUnderDirectory(
        const fs::path& directory,
        const fs::path& composed)
    {
        const fs::path root = directory.lexically_normal();
        const fs::path full = composed.lexically_normal();
        if (root.empty() || full.empty())
            return false;

        const fs::path relative = full.lexically_relative(root);
        if (relative.empty() || relative == fs::path("."))
            return false;

        for (const auto& part : relative) {
            if (part == "..")
                return false;
        }
        return true;
    }

    ResolveTelegramFileResult mapLookupError(const TelegramArchiveLookupResult& lookup)
    {
        return fail(
            lookup.error.value_or(ErrorCode::InternalError),
            lookup.diagnostic);
    }

    ResolveTelegramFileResult openTelegramFile(
        const std::string& directoryUtf8,
        const std::string& nameUtf8,
        ErrorCode missingFileError)
    {
        if (directoryUtf8.empty()) {
            return fail(
                ErrorCode::DatabaseSchemaFailed,
                "archive row has empty DirectTo");
        }
        if (nameUtf8.empty()) {
            return fail(
                ErrorCode::DatabaseSchemaFailed,
                "archive row has empty FileName");
        }

        fs::path directory;
        fs::path composed;
        try {
            directory = encoding::utf8_to_path(directoryUtf8);
            composed = directory / encoding::utf8_to_wstring(nameUtf8);
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::DatabaseSchemaFailed, error.what());
        }

        if (!compositionStaysUnderDirectory(directory, composed)) {
            return fail(
                ErrorCode::DatabaseSchemaFailed,
                "archive FileName escapes DirectTo");
        }

#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(composed.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_FILE_NOT_FOUND ||
                lastError == ERROR_PATH_NOT_FOUND) {
                return fail(missingFileError, encoding::path_to_utf8(composed));
            }
            return fail(
                ErrorCode::FileMetadataFailed,
                "GetFileAttributesW failed: " + std::to_string(lastError));
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            return fail(
                ErrorCode::FileOpenFailed,
                "telegram file is a reparse point");
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return fail(
                ErrorCode::FileOpenFailed,
                "telegram path is not a regular file");
        }
#else
        std::error_code metadataError;
        const auto status = fs::symlink_status(composed, metadataError);
        if (metadataError) {
            if (metadataError == std::errc::no_such_file_or_directory) {
                return fail(missingFileError, encoding::path_to_utf8(composed));
            }
            return fail(ErrorCode::FileMetadataFailed, metadataError.message());
        }
        if (!fs::exists(status)) {
            return fail(missingFileError, encoding::path_to_utf8(composed));
        }
        if (fs::is_symlink(status)) {
            return fail(
                ErrorCode::FileOpenFailed,
                "telegram file is a reparse point");
        }
        if (!fs::is_regular_file(status)) {
            return fail(
                ErrorCode::FileOpenFailed,
                "telegram path is not a regular file");
        }
#endif

        auto stream = std::make_shared<std::ifstream>(composed, std::ios::binary);
        if (!stream || !stream->is_open()) {
            return fail(
                ErrorCode::FileOpenFailed,
                encoding::path_to_utf8(composed));
        }

        stream->seekg(0, std::ios::end);
        const auto endPosition = stream->tellg();
        stream->seekg(0, std::ios::beg);
        const auto endOffset = static_cast<std::streamoff>(endPosition);
        if (endOffset < 0 || !*stream) {
            return fail(
                ErrorCode::FileMetadataFailed,
                encoding::path_to_utf8(composed));
        }

        ResolvedTelegramFile resolved;
        resolved.stream = std::move(stream);
        resolved.size = static_cast<std::uint64_t>(endOffset);
        resolved.path = std::move(composed);

        ResolveTelegramFileResult result;
        result.file = std::move(resolved);
        return result;
    }

    TelegramArchiveLookupResult ensureSourceLoaded(Telega::TYPE type)
    {
        try {
            Telega::ensureBasesLoaded(type);
        }
        catch (const Telega::SourceError& error) {
            const auto mapped = mapAutoPadSourceError(error);
            return lookupFail(
                mapped.error.value_or(ErrorCode::InternalError),
                mapped.diagnostic);
        }
        return {};
    }
}

TelegramArchiveLookupResult lookupTelegramArchive(
    int telegramId,
    Telega::TYPE type)
{
    const auto* bases = type == Telega::TYPE::VHOD
        ? &Telega::b_prm
        : &Telega::b_prd;

    const std::string sql =
        "SELECT PrilName, DirectTo, FileName FROM archive WHERE `index` = " +
        std::to_string(telegramId);

    for (const auto& baseName : *bases) {
        std::shared_ptr<mySQLite> database;
        try {
            database = SQLiteConnectionManager::instance().getReadOnlyConnection(baseName);
        }
        catch (const SQLiteOpenError& error) {
            return lookupFail(ErrorCode::DatabaseOpenFailed, error.what());
        }
        catch (const std::exception& error) {
            return lookupFail(ErrorCode::DatabaseOpenFailed, error.what());
        }

        mySQLite::RowList rows;
        try {
            rows = database->queryRows(sql);
        }
        catch (const SQLiteQueryError& error) {
            return lookupFail(
                error.isSchemaFailure()
                    ? ErrorCode::DatabaseSchemaFailed
                    : ErrorCode::DatabaseQueryFailed,
                error.what());
        }
        catch (const std::exception& error) {
            return lookupFail(ErrorCode::DatabaseQueryFailed, error.what());
        }

        if (rows.empty())
            continue;

        const auto& row = rows.front();
        const auto names = row.find("PrilName");
        const auto directory = row.find("DirectTo");
        const auto fileName = row.find("FileName");
        if (names == row.end() || directory == row.end() || fileName == row.end()) {
            return lookupFail(
                ErrorCode::DatabaseSchemaFailed,
                "archive row has no PrilName, DirectTo, or FileName column");
        }

        TelegramArchiveLookupResult result;
        result.record = TelegramArchiveRecord{
            names->second,
            directory->second,
            fileName->second};
        return result;
    }

    return {};
}

ResolveTelegramFileResult TelegramFileResolver::resolveAttachment(
    const std::vector<std::uint8_t>& request)
{
    ResolveTelegramFileResult parseError;
    const auto jsonRequest = parseRequestObject(request, parseError);
    if (!jsonRequest)
        return parseError;

    if (jsonContainsForbiddenFilesystemField(*jsonRequest)) {
        return fail(
            ErrorCode::InvalidRequest,
            "attachment request must not contain a server filesystem path");
    }

    int telegramId = 0;
    Telega::TYPE telegramType{};
    std::string diagnostic;
    if (!parseTelegramIdType(*jsonRequest, telegramId, telegramType, diagnostic)) {
        return fail(ErrorCode::InvalidRequest, std::move(diagnostic));
    }

    if (!jsonRequest->contains("file_name") ||
        !(*jsonRequest)["file_name"].is_string()) {
        return fail(
            ErrorCode::InvalidRequest,
            "single attachment request requires integer id/type and file_name");
    }

    std::string fileName;
    try {
        fileName = (*jsonRequest)["file_name"].get<std::string>();
    }
    catch (const nh::json::exception& error) {
        return fail(ErrorCode::InvalidRequest, error.what());
    }

    if (!isValidAttachmentBaseName(fileName)) {
        return fail(
            ErrorCode::InvalidRequest,
            "file_name must contain only an attachment basename");
    }

    if (!Telega::isSourceConfigured(telegramType)) {
        return fail(
            ErrorCode::AttachmentNotFound,
            "telegram has no attachments");
    }

    const auto loaded = ensureSourceLoaded(telegramType);
    if (loaded.failed())
        return mapLookupError(loaded);

    const auto lookup = lookupTelegramArchive(telegramId, telegramType);
    if (lookup.failed())
        return mapLookupError(lookup);
    if (!lookup.record || lookup.record->prilName.empty()) {
        return fail(
            ErrorCode::AttachmentNotFound,
            "telegram has no attachments");
    }

    if (!isAdvertisedAttachmentName(
            telegramType,
            telegramId,
            lookup.record->prilName,
            fileName)) {
        return fail(
            ErrorCode::AttachmentNotFound,
            "requested file is not advertised by the telegram");
    }

    return openTelegramFile(
        lookup.record->directTo,
        fileName,
        ErrorCode::AttachmentNotFound);
}

ResolveTelegramFileResult TelegramFileResolver::resolveText(
    const std::vector<std::uint8_t>& request)
{
    ResolveTelegramFileResult parseError;
    const auto jsonRequest = parseRequestObject(request, parseError);
    if (!jsonRequest)
        return parseError;

    if (jsonContainsForbiddenFilesystemField(*jsonRequest)) {
        return fail(
            ErrorCode::InvalidRequest,
            "text request must not contain a server filesystem path");
    }

    int telegramId = 0;
    Telega::TYPE telegramType{};
    std::string diagnostic;
    if (!parseTelegramIdType(*jsonRequest, telegramId, telegramType, diagnostic)) {
        return fail(ErrorCode::InvalidRequest, std::move(diagnostic));
    }

    const auto loaded = ensureSourceLoaded(telegramType);
    if (loaded.failed())
        return mapLookupError(loaded);

    const auto lookup = lookupTelegramArchive(telegramId, telegramType);
    if (lookup.failed())
        return mapLookupError(lookup);
    if (!lookup.record) {
        return fail(ErrorCode::FileNotFound, "telegram archive row was not found");
    }
    if (lookup.record->directTo.empty() || lookup.record->fileName.empty()) {
        return fail(
            ErrorCode::DatabaseSchemaFailed,
            "archive row has empty DirectTo or FileName");
    }
    if (dbFileNameEscapesDirectory(lookup.record->fileName)) {
        return fail(
            ErrorCode::DatabaseSchemaFailed,
            "archive FileName is absolute or rooted");
    }

    return openTelegramFile(
        lookup.record->directTo,
        lookup.record->fileName,
        ErrorCode::FileNotFound);
}
