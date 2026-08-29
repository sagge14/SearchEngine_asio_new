#include "StreamingUpload.h"

#include "MyUtils/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace streaming_upload
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;
    using command_execution::CommandResult;
    using command_execution::ErrorCode;

    namespace
    {
        CommandResult fail(ErrorCode error, std::string diagnostic)
        {
            return CommandResult::failure(error, std::move(diagnostic));
        }

        bool hasAsciiControlOrForbidden(std::string_view name)
        {
            for (const unsigned char character : name) {
                if (character < 32 || character == 127)
                    return true;
                switch (character) {
                    case '/':
                    case '\\':
                    case ':':
                    case '<':
                    case '>':
                    case '"':
                    case '|':
                    case '?':
                    case '*':
                        return true;
                    default:
                        break;
                }
            }
            return false;
        }

        std::wstring toUpperAsciiAware(std::wstring value)
        {
#ifdef _WIN32
            if (!value.empty()) {
                CharUpperBuffW(value.data(), static_cast<DWORD>(value.size()));
            }
#else
            for (wchar_t& character : value) {
                if (character >= L'a' && character <= L'z')
                    character = static_cast<wchar_t>(character - L'a' + L'A');
            }
#endif
            return value;
        }

        bool isReservedDeviceStem(std::wstring stem)
        {
            while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.'))
                stem.pop_back();
            stem = toUpperAsciiAware(std::move(stem));
            if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL")
                return true;
            if (stem.size() == 4) {
                const std::wstring prefix = stem.substr(0, 3);
                const wchar_t digit = stem[3];
                if ((prefix == L"COM" || prefix == L"LPT") &&
                    digit >= L'1' && digit <= L'9') {
                    return true;
                }
            }
            return false;
        }

        bool isReservedWindowsDeviceName(const std::wstring& filename)
        {
            if (isReservedDeviceStem(filename))
                return true;
            const std::wstring stem = fs::path(filename).stem().wstring();
            if (isReservedDeviceStem(stem))
                return true;
            const auto dot = filename.find(L'.');
            if (dot != std::wstring::npos &&
                isReservedDeviceStem(filename.substr(0, dot))) {
                return true;
            }
            return false;
        }

#ifdef _WIN32
        CommandResult checkExistingPathAttributes(
            const fs::path& path,
            bool mustBeDirectory)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD lastError = GetLastError();
                if (lastError == ERROR_FILE_NOT_FOUND ||
                    lastError == ERROR_PATH_NOT_FOUND) {
                    return CommandResult::success();
                }
                return fail(
                    ErrorCode::FileMetadataFailed,
                    "GetFileAttributesW failed: " + std::to_string(lastError));
            }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                return fail(
                    ErrorCode::FileOpenFailed,
                    "path is a reparse point");
            }
            const bool isDirectory =
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (mustBeDirectory && !isDirectory) {
                return fail(
                    ErrorCode::ConfigurationError,
                    "path is not a directory");
            }
            if (!mustBeDirectory && isDirectory) {
                return fail(
                    ErrorCode::FileOpenFailed,
                    "path is a directory");
            }
            return CommandResult::success();
        }
#else
        CommandResult checkExistingPathAttributes(
            const fs::path& path,
            bool mustBeDirectory)
        {
            std::error_code error;
            const auto status = fs::symlink_status(path, error);
            if (error) {
                if (error == std::errc::no_such_file_or_directory)
                    return CommandResult::success();
                return fail(ErrorCode::FileMetadataFailed, error.message());
            }
            if (!fs::exists(status))
                return CommandResult::success();
            if (fs::is_symlink(status)) {
                return fail(ErrorCode::FileOpenFailed, "path is a reparse point");
            }
            if (mustBeDirectory && !fs::is_directory(status)) {
                return fail(
                    ErrorCode::ConfigurationError,
                    "path is not a directory");
            }
            if (!mustBeDirectory && !fs::is_regular_file(status)) {
                return fail(ErrorCode::FileOpenFailed, "path is not a regular file");
            }
            return CommandResult::success();
        }
#endif

        CommandResult ensureDirectoryExistsSafe(const fs::path& directory)
        {
            std::error_code error;
            if (!fs::exists(directory, error)) {
                if (error) {
                    return fail(ErrorCode::FileMetadataFailed, error.message());
                }
                fs::create_directories(directory, error);
                if (error) {
                    return fail(
                        ErrorCode::DirectoryCreateFailed,
                        error.message());
                }
            }
            return checkExistingPathAttributes(directory, true);
        }

        CommandResult ensureSafeDirectoryTree(
            const fs::path& root,
            const fs::path& directory)
        {
            if (!compositionStaysUnderDirectory(root, directory) &&
                fs::path(directory).lexically_normal() !=
                    fs::path(root).lexically_normal()) {
                return fail(
                    ErrorCode::ConfigurationError,
                    "destination escapes configured root");
            }

            const fs::path normalRoot = root.lexically_normal();
            const fs::path normalDir = directory.lexically_normal();
            auto check = ensureDirectoryExistsSafe(normalRoot);
            if (check.failed())
                return check;

            fs::path current = normalRoot;
            const fs::path relative = normalDir.lexically_relative(normalRoot);
            if (relative.empty() || relative == fs::path("."))
                return CommandResult::success();

            for (const auto& part : relative) {
                if (part.empty() || part == "." || part == "..") {
                    return fail(
                        ErrorCode::ConfigurationError,
                        "unsafe destination directory component");
                }
                current /= part;
                check = ensureDirectoryExistsSafe(current);
                if (check.failed())
                    return check;
            }
            return CommandResult::success();
        }

        bool isNumberIntegerNotFloat(const nh::json& value)
        {
            return value.is_number_integer() && !value.is_number_float();
        }

        std::wstring nextStagingLeaf()
        {
            static std::atomic<std::uint64_t> sequence{0};
            const auto next = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef _WIN32
            const auto pid = static_cast<unsigned long>(_getpid());
#else
            const auto pid = static_cast<unsigned long>(getpid());
#endif
            const auto ticks =
                static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            std::wostringstream name;
            name << L".se-upload-" << pid << L"-" << ticks << L"-" << next
                 << L".tmp";
            return name.str();
        }

        std::wstring suffixedFileName(const std::wstring& filename, int index)
        {
            if (index <= 0)
                return filename;
            const fs::path asPath(filename);
            std::wostringstream name;
            name << asPath.stem().wstring() << L"(" << index << L")"
                 << asPath.extension().wstring();
            return name.str();
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
        if (full == root)
            return true;

        const fs::path relative = full.lexically_relative(root);
        if (relative.empty() || relative == fs::path("."))
            return full == root;

        for (const auto& part : relative) {
            if (part == "..")
                return false;
        }
        return true;
    }

    bool isSafeFileComponent(std::string_view name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        if (name.find('\0') != std::string_view::npos)
            return false;
        if (hasAsciiControlOrForbidden(name))
            return false;
        if (name.back() == ' ' || name.back() == '.')
            return false;

        try {
            const fs::path path = encoding::utf8_to_path(std::string(name));
            if (path.empty() ||
                path.is_absolute() ||
                path.has_root_path() ||
                path.has_root_name() ||
                path.has_root_directory() ||
                path.has_parent_path() ||
                path.filename() != path) {
                return false;
            }
            if (isReservedWindowsDeviceName(path.wstring()))
                return false;
        }
        catch (...) {
            return false;
        }
        return true;
    }

    bool isSafeBasename(std::string_view name)
    {
        return isSafeFileComponent(name);
    }

    bool isSafeOperatorComponent(std::string_view name)
    {
        return isSafeFileComponent(name);
    }

    CommandResult parseMetadata(std::span<const std::uint8_t> bytes, Metadata& out)
    {
        out = Metadata{};
        if (bytes.size() > kMaxMetadataBytes) {
            return fail(ErrorCode::PayloadTooLarge, "metadata exceeds 16 KiB");
        }

        nh::json document;
        try {
            document = nh::json::parse(bytes.begin(), bytes.end());
        }
        catch (const nh::json::parse_error& error) {
            return fail(ErrorCode::InvalidJson, error.what());
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidJson, error.what());
        }

        if (!document.is_object() || document.size() != 3) {
            return fail(
                ErrorCode::InvalidRequest,
                "metadata must contain exactly version, file_name, file_size");
        }
        if (!document.contains("version") ||
            !document.contains("file_name") ||
            !document.contains("file_size")) {
            return fail(
                ErrorCode::InvalidRequest,
                "metadata missing required field");
        }
        if (!isNumberIntegerNotFloat(document["version"])) {
            return fail(ErrorCode::InvalidRequest, "version must be an integer");
        }
        std::uint64_t version = 0;
        try {
            if (!document["version"].is_number_unsigned() &&
                document["version"].get<std::int64_t>() < 0) {
                return fail(ErrorCode::InvalidRequest, "version must be unsigned");
            }
            version = document["version"].get<std::uint64_t>();
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidRequest, error.what());
        }
        if (version != kProtocolVersion) {
            return fail(ErrorCode::InvalidRequest, "unsupported metadata version");
        }
        if (!document["file_name"].is_string()) {
            return fail(ErrorCode::InvalidRequest, "file_name must be a string");
        }
        if (!isNumberIntegerNotFloat(document["file_size"])) {
            return fail(ErrorCode::InvalidRequest, "file_size must be an integer");
        }
        std::uint64_t fileSize = 0;
        try {
            if (!document["file_size"].is_number_unsigned() &&
                document["file_size"].get<std::int64_t>() < 0) {
                return fail(ErrorCode::InvalidRequest, "file_size must be unsigned");
            }
            fileSize = document["file_size"].get<std::uint64_t>();
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidRequest, error.what());
        }

        const std::string fileName = document["file_name"].get<std::string>();
        if (!isSafeBasename(fileName)) {
            return fail(ErrorCode::InvalidRequest, "unsafe file_name");
        }

        out.version = kProtocolVersion;
        out.file_name = fileName;
        out.file_size = fileSize;
        return CommandResult::success();
    }

    TimeParts currentLocalTimeParts()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &nowTime);
#else
        localtime_r(&nowTime, &localTime);
#endif

        static const std::map<int, std::string> monthNames = {
            {1, "ЯНВАРЬ"}, {2, "ФЕВРАЛЬ"}, {3, "МАРТ"}, {4, "АПРЕЛЬ"},
            {5, "МАЙ"}, {6, "ИЮНЬ"}, {7, "ИЮЛЬ"}, {8, "АВГУСТ"},
            {9, "СЕНТЯБРЬ"}, {10, "ОКТЯБРЬ"}, {11, "НОЯБРЬ"}, {12, "ДЕКАБРЬ"}};

        TimeParts parts;
        parts.monthUpper = encoding::utf8_to_wstring(
            monthNames.at(localTime.tm_mon + 1));
        std::wostringstream dateStream;
        dateStream << std::put_time(&localTime, L"%d.%m.%y");
        parts.date = dateStream.str();
        std::wostringstream timeStream;
        timeStream << std::put_time(&localTime, L"%H-%M");
        parts.hhmm = timeStream.str();
        return parts;
    }

    CommandResult planRaznTarget(
        const fs::path& raznRoot,
        const Metadata& metadata,
        PlannedTarget& out)
    {
        out = PlannedTarget{};
        if (raznRoot.empty() || !raznRoot.is_absolute()) {
            return fail(
                ErrorCode::ConfigurationError,
                "razn_output_dir is missing or not absolute");
        }
        if (!isSafeBasename(metadata.file_name)) {
            return fail(ErrorCode::InvalidRequest, "unsafe file_name");
        }

        fs::path directory;
        fs::path composed;
        try {
            directory = raznRoot.lexically_normal();
            composed = directory / encoding::utf8_to_wstring(metadata.file_name);
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidRequest, error.what());
        }
        if (!compositionStaysUnderDirectory(directory, composed)) {
            return fail(
                ErrorCode::ConfigurationError,
                "RAZN destination escapes razn_output_dir");
        }

        auto created = ensureSafeDirectoryTree(directory, directory);
        if (created.failed())
            return created;

        out.directory = directory;
        out.baseName = metadata.file_name;
        return CommandResult::success();
    }

    CommandResult planTlgTarget(
        const fs::path& tlgRoot,
        std::string_view operatorName,
        const Metadata& metadata,
        const TimeParts& time,
        PlannedTarget& out)
    {
        out = PlannedTarget{};
        if (tlgRoot.empty() || !tlgRoot.is_absolute()) {
            return fail(
                ErrorCode::ConfigurationError,
                "tlg_send_root is missing or not absolute");
        }
        if (!isSafeOperatorComponent(operatorName)) {
            return fail(
                ErrorCode::OperatorNotRegistered,
                "authenticated operator is not a safe directory component");
        }
        if (!isSafeBasename(metadata.file_name)) {
            return fail(ErrorCode::InvalidRequest, "unsafe file_name");
        }
        if (time.monthUpper.empty() || time.date.empty() || time.hhmm.empty()) {
            return fail(ErrorCode::InternalError, "upload time parts are empty");
        }

        try {
            const std::string monthUtf8 = encoding::wstring_to_utf8(time.monthUpper);
            const std::string dateUtf8 = encoding::wstring_to_utf8(time.date);
            const std::string hhmmUtf8 = encoding::wstring_to_utf8(time.hhmm);
            if (!isSafeFileComponent(monthUtf8) ||
                !isSafeFileComponent(dateUtf8)) {
                return fail(ErrorCode::InternalError, "generated date folder is unsafe");
            }
            const std::string operatorFolder =
                std::string(operatorName) + "_" + hhmmUtf8;
            if (!isSafeFileComponent(operatorFolder)) {
                return fail(
                    ErrorCode::OperatorNotRegistered,
                    "operator time folder is not a safe directory component");
            }

            const fs::path root = tlgRoot.lexically_normal();
            const fs::path directory = root / time.monthUpper / time.date /
                encoding::utf8_to_wstring(operatorFolder);
            const fs::path composed =
                directory / encoding::utf8_to_wstring(metadata.file_name);
            if (!compositionStaysUnderDirectory(root, directory) ||
                !compositionStaysUnderDirectory(root, composed)) {
                return fail(
                    ErrorCode::ConfigurationError,
                    "TLG destination escapes tlg_send_root");
            }

            auto created = ensureSafeDirectoryTree(root, directory);
            if (created.failed())
                return created;

            out.directory = directory;
            out.baseName = metadata.file_name;
            return CommandResult::success();
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidRequest, error.what());
        }
    }

    std::string makeReadyJson()
    {
        return nh::json{
            {"version", kProtocolVersion},
            {"ready", true}}.dump();
    }

    std::string makeFinalJson(std::string_view savedName)
    {
        return nh::json{
            {"version", kProtocolVersion},
            {"ok", true},
            {"saved_name", std::string(savedName)}}.dump();
    }

    StreamingUploadSink::~StreamingUploadSink()
    {
        abort();
    }

    void StreamingUploadSink::moveFrom(StreamingUploadSink& other) noexcept
    {
        destinationDir_ = std::move(other.destinationDir_);
        stagingPath_ = std::move(other.stagingPath_);
        publishedPath_ = std::move(other.publishedPath_);
        requestedFileName_ = std::move(other.requestedFileName_);
        savedName_ = std::move(other.savedName_);
        advertisedSize_ = other.advertisedSize_;
        bytesWritten_ = other.bytesWritten_;
        prepared_ = other.prepared_;
        published_ = other.published_;
#ifdef _WIN32
        stagingHandle_ = other.stagingHandle_;
        other.stagingHandle_ = INVALID_HANDLE_VALUE;
#else
        stagingFile_ = other.stagingFile_;
        other.stagingFile_ = nullptr;
#endif
        other.prepared_ = false;
        other.published_ = false;
        other.advertisedSize_ = 0;
        other.bytesWritten_ = 0;
    }

    StreamingUploadSink::StreamingUploadSink(StreamingUploadSink&& other) noexcept
    {
        moveFrom(other);
    }

    StreamingUploadSink& StreamingUploadSink::operator=(
        StreamingUploadSink&& other) noexcept
    {
        if (this != &other) {
            abort();
            moveFrom(other);
        }
        return *this;
    }

    void StreamingUploadSink::closeStagingHandle() noexcept
    {
#ifdef _WIN32
        if (stagingHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stagingHandle_);
            stagingHandle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (stagingFile_ != nullptr) {
            std::fclose(stagingFile_);
            stagingFile_ = nullptr;
        }
#endif
    }

    void StreamingUploadSink::abort() noexcept
    {
        closeStagingHandle();
        if (prepared_ && !published_ && !stagingPath_.empty()) {
            std::error_code ignored;
            fs::remove(stagingPath_, ignored);
        }
        prepared_ = false;
        published_ = false;
        stagingPath_.clear();
        bytesWritten_ = 0;
    }

    CommandResult StreamingUploadSink::prepare(
        const PlannedTarget& target,
        std::uint64_t fileSize)
    {
        abort();
        if (target.directory.empty() || target.baseName.empty()) {
            return fail(ErrorCode::InternalError, "upload target is empty");
        }
        if (!isSafeBasename(target.baseName)) {
            return fail(ErrorCode::InvalidRequest, "unsafe file_name");
        }

        try {
            requestedFileName_ = encoding::utf8_to_wstring(target.baseName);
        }
        catch (const std::exception& error) {
            return fail(ErrorCode::InvalidRequest, error.what());
        }

        const fs::path composed = target.directory / requestedFileName_;
        if (!compositionStaysUnderDirectory(target.directory, composed)) {
            return fail(
                ErrorCode::ConfigurationError,
                "upload target escapes destination directory");
        }

        auto dirs = ensureSafeDirectoryTree(target.directory, target.directory);
        if (dirs.failed())
            return dirs;

#ifdef _WIN32
        HANDLE handle = INVALID_HANDLE_VALUE;
        fs::path staging;
        for (int attempt = 0; attempt < 256; ++attempt) {
            staging = target.directory / nextStagingLeaf();
            if (!compositionStaysUnderDirectory(target.directory, staging)) {
                return fail(
                    ErrorCode::ConfigurationError,
                    "staging path escapes destination directory");
            }
            handle = CreateFileW(
                staging.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY,
                nullptr);
            if (handle != INVALID_HANDLE_VALUE)
                break;
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS)
                continue;
            return fail(
                ErrorCode::FileOpenFailed,
                "failed to create staging file: " + std::to_string(lastError));
        }
        if (handle == INVALID_HANDLE_VALUE) {
            return fail(ErrorCode::FileOpenFailed, "failed to choose unique staging name");
        }
        const auto stagingCheck = checkExistingPathAttributes(staging, false);
        if (stagingCheck.failed()) {
            CloseHandle(handle);
            std::error_code ignored;
            fs::remove(staging, ignored);
            return stagingCheck;
        }
        stagingHandle_ = handle;
#else
        fs::path staging;
        std::FILE* file = nullptr;
        for (int attempt = 0; attempt < 256; ++attempt) {
            staging = target.directory / nextStagingLeaf();
            file = std::fopen(staging.string().c_str(), "wb");
            if (file != nullptr)
                break;
        }
        if (file == nullptr) {
            return fail(ErrorCode::FileOpenFailed, "failed to create staging file");
        }
        stagingFile_ = file;
#endif

        destinationDir_ = target.directory;
        stagingPath_ = std::move(staging);
        advertisedSize_ = fileSize;
        bytesWritten_ = 0;
        savedName_.clear();
        publishedPath_.clear();
        prepared_ = true;
        published_ = false;
        return CommandResult::success();
    }

    CommandResult StreamingUploadSink::writeChunk(std::span<const std::uint8_t> chunk)
    {
        if (!prepared_ || published_) {
            return fail(ErrorCode::InternalError, "upload sink is not writable");
        }
        if (chunk.empty())
            return CommandResult::success();
        if (bytesWritten_ > advertisedSize_ ||
            chunk.size() > advertisedSize_ - bytesWritten_) {
            return fail(ErrorCode::InvalidRequest, "upload exceeded advertised file_size");
        }

#ifdef _WIN32
        if (stagingHandle_ == INVALID_HANDLE_VALUE) {
            return fail(ErrorCode::FileOpenFailed, "staging handle is closed");
        }
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const DWORD toWrite = static_cast<DWORD>(
                (std::min)(chunk.size() - offset, static_cast<std::size_t>(64u * 1024u)));
            DWORD written = 0;
            if (!WriteFile(
                    stagingHandle_,
                    chunk.data() + offset,
                    toWrite,
                    &written,
                    nullptr) ||
                written != toWrite) {
                return fail(ErrorCode::FileWriteFailed, "staging WriteFile failed");
            }
            offset += written;
        }
#else
        if (stagingFile_ == nullptr) {
            return fail(ErrorCode::FileOpenFailed, "staging file is closed");
        }
        if (std::fwrite(chunk.data(), 1, chunk.size(), stagingFile_) != chunk.size()) {
            return fail(ErrorCode::FileWriteFailed, "staging fwrite failed");
        }
#endif
        bytesWritten_ += chunk.size();
        return CommandResult::success();
    }

    CommandResult StreamingUploadSink::publish()
    {
        if (!prepared_ || published_) {
            return fail(ErrorCode::InternalError, "upload sink is not ready to publish");
        }
        if (bytesWritten_ != advertisedSize_) {
            return fail(
                ErrorCode::FileWriteFailed,
                "advertised file_size was not fully received");
        }

#ifdef _WIN32
        if (stagingHandle_ != INVALID_HANDLE_VALUE) {
            if (!FlushFileBuffers(stagingHandle_)) {
                return fail(ErrorCode::FileWriteFailed, "FlushFileBuffers failed");
            }
        }
#else
        if (stagingFile_ != nullptr && std::fflush(stagingFile_) != 0) {
            return fail(ErrorCode::FileWriteFailed, "fflush failed");
        }
#endif
        closeStagingHandle();

        for (int index = 0; index < 100000; ++index) {
            const std::wstring candidateName = suffixedFileName(requestedFileName_, index);
            const fs::path dest = destinationDir_ / candidateName;
            if (!compositionStaysUnderDirectory(destinationDir_, dest)) {
                abort();
                return fail(
                    ErrorCode::ConfigurationError,
                    "publish candidate escapes destination directory");
            }

#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(dest.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                continue;
            }
            if (!MoveFileExW(stagingPath_.c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const DWORD lastError = GetLastError();
                if (lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS)
                    continue;
                abort();
                return fail(
                    ErrorCode::FileWriteFailed,
                    "MoveFileExW failed: " + std::to_string(lastError));
            }
            const auto publishedCheck = checkExistingPathAttributes(dest, false);
            if (publishedCheck.failed()) {
                std::error_code ignored;
                fs::remove(dest, ignored);
                abort();
                return publishedCheck;
            }
#else
            std::error_code existsError;
            if (fs::exists(dest, existsError))
                continue;
            fs::rename(stagingPath_, dest, existsError);
            if (existsError) {
                if (existsError == std::errc::file_exists)
                    continue;
                abort();
                return fail(ErrorCode::FileWriteFailed, existsError.message());
            }
#endif
            try {
                savedName_ = encoding::wstring_to_utf8(candidateName);
            }
            catch (const std::exception& error) {
                abort();
                return fail(ErrorCode::SerializationFailed, error.what());
            }
            publishedPath_ = dest;
            stagingPath_.clear();
            prepared_ = false;
            published_ = true;
            return CommandResult::success();
        }

        abort();
        return fail(ErrorCode::FileWriteFailed, "failed to choose a unique published name");
    }
}
