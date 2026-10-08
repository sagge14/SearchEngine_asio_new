#include "PdtvFileResolver.h"

#include "Commands/GetJsonTelega/AutoPadSource.h"
#include "MyUtils/Utf8Path.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "SQLite/mySQLite.h"
#include "nlohmann/json.hpp"

#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    bool jsonContainsForbiddenFilesystemField(const nh::json& request)
    {
        for (const char* key : kForbiddenFilesystemKeys) {
            if (request.contains(key))
                return true;
        }
        return false;
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

    bool parseNonNegativeIntField(
        const nh::json& request,
        const char* key,
        int& value,
        std::string& diagnostic)
    {
        if (!request.contains(key) || !request[key].is_number_integer()) {
            diagnostic = std::string("request requires integer ") + key;
            return false;
        }

        std::int64_t wireValue{};
        try {
            wireValue = request[key].get<std::int64_t>();
        }
        catch (const nh::json::exception& error) {
            diagnostic = error.what();
            return false;
        }

        if (wireValue < 0 || wireValue > (std::numeric_limits<int>::max)()) {
            diagnostic = std::string(key) + " is outside the supported range";
            return false;
        }

        value = static_cast<int>(wireValue);
        return true;
    }

    std::vector<std::string> splitLegacy(const std::string& text, char delimiter)
    {
        std::vector<std::string> parts;
        std::string current;
        for (const char ch : text) {
            if (ch == delimiter) {
                parts.push_back(std::move(current));
                current.clear();
            }
            else {
                current.push_back(ch);
            }
        }
        parts.push_back(std::move(current));
        return parts;
    }

    std::string normalizeLegacyPdtvPath(std::string path)
    {
        if (path.find('\0') != std::string::npos)
            return {};

        const auto semicolon = path.find(';');
        if (semicolon != std::string::npos)
            path.erase(semicolon);

        if (!path.empty() && path.back() == '/')
            path.pop_back();

        return path;
    }

    const std::string* findRowField(
        const mySQLite::Row& row,
        std::initializer_list<const char*> names)
    {
        for (const char* name : names) {
            const auto it = row.find(name);
            if (it != row.end())
                return &it->second;
        }
        return nullptr;
    }

    ResolveTelegramFileResult openPdtvFile(const std::string& pathUtf8)
    {
        if (pathUtf8.find('\0') != std::string::npos) {
            return fail(
                ErrorCode::DatabaseSchemaFailed,
                "PDTV path contains an embedded NUL");
        }

        fs::path path;
        try {
            path = encoding::utf8_to_path(pathUtf8);
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::DatabaseSchemaFailed, error.what());
        }

        if (path.empty()) {
            return fail(
                ErrorCode::DatabaseSchemaFailed,
                "PDTV path is empty after normalization");
        }

#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_FILE_NOT_FOUND ||
                lastError == ERROR_PATH_NOT_FOUND) {
                return fail(ErrorCode::FileNotFound, encoding::path_to_utf8(path));
            }
            return fail(
                ErrorCode::FileMetadataFailed,
                "GetFileAttributesW failed: " + std::to_string(lastError));
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            return fail(
                ErrorCode::FileOpenFailed,
                "PDTV file is a reparse point");
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return fail(
                ErrorCode::FileOpenFailed,
                "PDTV path is not a regular file");
        }
#else
        std::error_code metadataError;
        const auto status = fs::symlink_status(path, metadataError);
        if (metadataError) {
            if (metadataError == std::errc::no_such_file_or_directory) {
                return fail(ErrorCode::FileNotFound, encoding::path_to_utf8(path));
            }
            return fail(ErrorCode::FileMetadataFailed, metadataError.message());
        }
        if (!fs::exists(status)) {
            return fail(ErrorCode::FileNotFound, encoding::path_to_utf8(path));
        }
        if (fs::is_symlink(status)) {
            return fail(
                ErrorCode::FileOpenFailed,
                "PDTV file is a reparse point");
        }
        if (!fs::is_regular_file(status)) {
            return fail(
                ErrorCode::FileOpenFailed,
                "PDTV path is not a regular file");
        }
#endif

        auto stream = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!stream || !stream->is_open()) {
            return fail(
                ErrorCode::FileOpenFailed,
                encoding::path_to_utf8(path));
        }

        stream->seekg(0, std::ios::end);
        const auto endPosition = stream->tellg();
        stream->seekg(0, std::ios::beg);
        const auto endOffset = static_cast<std::streamoff>(endPosition);
        if (endOffset < 0 || !*stream) {
            return fail(
                ErrorCode::FileMetadataFailed,
                encoding::path_to_utf8(path));
        }

        ResolvedTelegramFile resolved;
        resolved.stream = std::move(stream);
        resolved.size = static_cast<std::uint64_t>(endOffset);
        resolved.path = std::move(path);

        ResolveTelegramFileResult result;
        result.file = std::move(resolved);
        return result;
    }

    ResolveTelegramFileResult lookupPdtvRow(
        int telegramId,
        std::string& pdtv,
        std::string& allPdtv1)
    {
        const std::string sql =
            "select pdtv, allpdtv1 from archive where `index` = " +
            std::to_string(telegramId);

        for (const auto& baseName : Telega::b_prd) {
            std::shared_ptr<mySQLite> database;
            try {
                database =
                    SQLiteConnectionManager::instance().getReadOnlyConnection(
                        baseName);
            }
            catch (const SQLiteOpenError& error) {
                return fail(ErrorCode::DatabaseOpenFailed, error.what());
            }
            catch (const std::exception& error) {
                return fail(ErrorCode::DatabaseOpenFailed, error.what());
            }

            mySQLite::RowList rows;
            try {
                rows = database->queryRows(sql);
            }
            catch (const SQLiteQueryError& error) {
                return fail(
                    error.isSchemaFailure()
                        ? ErrorCode::DatabaseSchemaFailed
                        : ErrorCode::DatabaseQueryFailed,
                    error.what());
            }
            catch (const std::exception& error) {
                return fail(ErrorCode::DatabaseQueryFailed, error.what());
            }

            if (rows.empty())
                continue;

            const auto& row = rows.front();
            const auto* pdtvField = findRowField(row, {"PDTV", "pdtv"});
            const auto* allField = findRowField(row, {"AllPDTV1", "allpdtv1"});
            if (pdtvField == nullptr || allField == nullptr) {
                return fail(
                    ErrorCode::DatabaseSchemaFailed,
                    "archive row has no PDTV or AllPDTV1 column");
            }

            pdtv = *pdtvField;
            allPdtv1 = *allField;
            return {};
        }

        return fail(ErrorCode::FileNotFound, "PDTV archive row was not found");
    }
}

ResolveTelegramFileResult PdtvFileResolver::resolve(
    const std::vector<std::uint8_t>& request)
{
    ResolveTelegramFileResult parseError;
    const auto jsonRequest = parseRequestObject(request, parseError);
    if (!jsonRequest)
        return parseError;

    if (!jsonRequest->is_object()) {
        return fail(ErrorCode::InvalidRequest, "PDTV request must be a JSON object");
    }

    if (jsonContainsForbiddenFilesystemField(*jsonRequest)) {
        return fail(
            ErrorCode::InvalidRequest,
            "PDTV request must not contain a server filesystem path");
    }

    int telegramId = 0;
    int slot = 0;
    int entryIndex = 0;
    std::string diagnostic;
    if (!parseNonNegativeIntField(*jsonRequest, "id", telegramId, diagnostic) ||
        !parseNonNegativeIntField(*jsonRequest, "slot", slot, diagnostic) ||
        !parseNonNegativeIntField(
            *jsonRequest, "entry_index", entryIndex, diagnostic)) {
        return fail(ErrorCode::InvalidRequest, std::move(diagnostic));
    }

    if (slot != 0 && slot != 1) {
        return fail(ErrorCode::InvalidRequest, "slot must be 0 or 1");
    }
    if (slot == 0 && entryIndex != 0) {
        return fail(
            ErrorCode::InvalidRequest,
            "slot 0 requires entry_index 0");
    }

    if (!Telega::isSourceConfigured(Telega::TYPE::ISHOD)) {
        return fail(
            ErrorCode::DataSourceDisabled,
            "PRD data source is disabled");
    }

    try {
        Telega::ensureBasesLoaded(Telega::TYPE::ISHOD);
    }
    catch (const Telega::SourceError& error) {
        const auto mapped = mapAutoPadSourceError(error);
        return fail(
            mapped.error.value_or(ErrorCode::InternalError),
            mapped.diagnostic);
    }

    std::string pdtv;
    std::string allPdtv1;
    const auto lookup = lookupPdtvRow(telegramId, pdtv, allPdtv1);
    if (lookup.error.has_value())
        return lookup;

    std::string rawEntry;
    if (slot == 0) {
        rawEntry = pdtv;
    }
    else {
        const auto entries = splitLegacy(allPdtv1, '/');
        if (entryIndex >= static_cast<int>(entries.size())) {
            return fail(
                ErrorCode::InvalidRequest,
                "entry_index is outside AllPDTV1 range");
        }
        rawEntry = entries[static_cast<std::size_t>(entryIndex)];
    }

    const auto parts = splitLegacy(rawEntry, '|');
    if (parts.size() != 3) {
        return fail(
            ErrorCode::DatabaseSchemaFailed,
            "PDTV metadata entry is not file|date_time|dir");
    }

    const auto path = normalizeLegacyPdtvPath(parts[2]);
    if (path.empty()) {
        return fail(
            ErrorCode::DatabaseSchemaFailed,
            "PDTV path is empty after parsing");
    }

    return openPdtvFile(path);
}
