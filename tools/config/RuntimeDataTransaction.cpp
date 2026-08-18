#include "RuntimeDataTransaction.h"

#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace runtime_data_transaction {
namespace {

using json = nlohmann::json;

constexpr int kFormatVersion = 1;
constexpr DWORD kCopyChunkBytes = 64 * 1024;
constexpr wchar_t kMarkerName[] = L".searchengine-runtime-update-marker";
constexpr wchar_t kManifestName[] = L"manifest.json";
constexpr wchar_t kRuntimeUpdatePrefix[] = L".runtime-update-";
constexpr wchar_t kSettingsName[] = L"Settings.json";
constexpr wchar_t kOemName[] = L"OEM866.INI";
constexpr wchar_t kEndpointName[] = L"client-endpoint.txt";
constexpr wchar_t kIgnoreName[] = L"ignore.txt";
constexpr wchar_t kLogsName[] = L"logs";
constexpr wchar_t kMessagesName[] = L"messages";
constexpr wchar_t kSettingsSnapshot[] = L"Settings.json.snapshot";
constexpr wchar_t kOemSnapshot[] = L"OEM866.INI.snapshot";
constexpr wchar_t kEndpointSnapshot[] = L"client-endpoint.txt.snapshot";

enum class PathKind {
    Missing,
    File,
    Directory,
    Reparse,
    Other
};

struct ManagedFileState {
    const wchar_t* name{};
    const wchar_t* snapshotName{};
    bool existed = false;
};

struct TransactionManifest {
    int formatVersion = 0;
    std::wstring transactionId;
    std::wstring dataDir;
    bool ignoreExisted = false;
    std::string ignoreSha256;
    ManagedFileState settings{kSettingsName, kSettingsSnapshot};
    ManagedFileState oem{kOemName, kOemSnapshot};
    ManagedFileState endpoint{kEndpointName, kEndpointSnapshot};
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const { return handle_; }
    bool valid() const
    {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
    HANDLE release()
    {
        const HANDLE value = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void reset()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class CryptProvider {
public:
    CryptProvider()
    {
        if (!CryptAcquireContextW(
                &provider_,
                nullptr,
                nullptr,
                PROV_RSA_AES,
                CRYPT_VERIFYCONTEXT))
        {
            throw std::runtime_error("cannot initialize SHA-256 provider");
        }
    }
    ~CryptProvider()
    {
        if (provider_ != 0) {
            CryptReleaseContext(provider_, 0);
        }
    }
    CryptProvider(const CryptProvider&) = delete;
    CryptProvider& operator=(const CryptProvider&) = delete;
    HCRYPTPROV get() const { return provider_; }

private:
    HCRYPTPROV provider_{0};
};

std::optional<std::wstring> option(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for command option");
            }
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::wstring requiredOption(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    const auto value = option(args, name);
    if (!value || value->empty()) {
        throw std::runtime_error("required command option is missing");
    }
    return *value;
}

std::string utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("cannot encode path as UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf16(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("cannot decode UTF-8 path");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

std::wstring joinPath(const std::wstring& left, const wchar_t* right)
{
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L'\\' + right;
}

std::wstring fileName(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::wstring parentPath(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    if (slash == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, slash);
}

std::wstring stripTrailingSlash(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring canonicalPath(const std::wstring& input)
{
    if (input.empty()) {
        throw std::runtime_error("path is empty");
    }
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        throw std::runtime_error(
            "cannot normalize path; Win32 error " + std::to_string(GetLastError()));
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) {
        throw std::runtime_error(
            "cannot normalize path; Win32 error " + std::to_string(GetLastError()));
    }
    buffer.resize(written);
    return stripTrailingSlash(std::move(buffer));
}

bool equalPath(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool isDriveRoot(const std::wstring& path)
{
    const std::wstring full = stripTrailingSlash(path);
    if (full.size() == 2 && full[1] == L':') {
        return true;
    }
    if (full.size() == 3 && full[1] == L':' &&
        (full[2] == L'\\' || full[2] == L'/'))
    {
        return true;
    }
    return false;
}

bool isInsideOrEqual(const std::wstring& child, const std::wstring& parent)
{
    if (equalPath(child, parent)) {
        return true;
    }
    if (child.size() <= parent.size()) {
        return false;
    }
    if (CompareStringOrdinal(
            child.c_str(), static_cast<int>(parent.size()),
            parent.c_str(), static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL)
    {
        return false;
    }
    const wchar_t next = child[parent.size()];
    return next == L'\\' || next == L'/';
}

PathKind classifyPath(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return PathKind::Missing;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return PathKind::Reparse;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        return PathKind::Directory;
    }
    if (attributes & FILE_ATTRIBUTE_DEVICE) {
        return PathKind::Other;
    }
    return PathKind::File;
}

void requireRegularFile(const std::wstring& path, const char* label)
{
    switch (classifyPath(path)) {
    case PathKind::File:
        return;
    case PathKind::Missing:
        throw std::runtime_error(std::string(label) + " is missing");
    case PathKind::Directory:
        throw std::runtime_error(std::string(label) + " is a directory");
    case PathKind::Reparse:
        throw std::runtime_error(std::string(label) + " is a reparse point");
    default:
        throw std::runtime_error(std::string(label) + " is not a regular file");
    }
}

void requireDirectory(const std::wstring& path, const char* label)
{
    switch (classifyPath(path)) {
    case PathKind::Directory:
        return;
    case PathKind::Missing:
        throw std::runtime_error(std::string(label) + " is missing");
    case PathKind::File:
        throw std::runtime_error(std::string(label) + " is a file");
    case PathKind::Reparse:
        throw std::runtime_error(std::string(label) + " is a reparse point");
    default:
        throw std::runtime_error(std::string(label) + " is not a directory");
    }
}

UniqueHandle openExistingFile(const std::wstring& path, DWORD access, DWORD share)
{
    const HANDLE handle = CreateFileW(
        path.c_str(),
        access,
        share,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "cannot open file; Win32 error " + std::to_string(GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::runtime_error(
            "cannot inspect file; Win32 error " + std::to_string(error));
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle(handle);
        throw std::runtime_error("refusing to follow a reparse point");
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        CloseHandle(handle);
        throw std::runtime_error("path is a directory");
    }
    return UniqueHandle(handle);
}

UniqueHandle createNewFile(const std::wstring& path)
{
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "cannot create new file; Win32 error " + std::to_string(GetLastError()));
    }
    return UniqueHandle(handle);
}

void copyHandleToHandle(HANDLE source, HANDLE destination)
{
    std::vector<char> buffer(kCopyChunkBytes);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(
                source, buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr))
        {
            throw std::runtime_error(
                "cannot read file; Win32 error " + std::to_string(GetLastError()));
        }
        if (read == 0) {
            break;
        }
        DWORD written = 0;
        DWORD offset = 0;
        while (offset < read) {
            if (!WriteFile(
                    destination,
                    buffer.data() + offset,
                    read - offset,
                    &written,
                    nullptr))
            {
                throw std::runtime_error(
                    "cannot write file; Win32 error " +
                    std::to_string(GetLastError()));
            }
            offset += written;
        }
    }
    if (!FlushFileBuffers(destination)) {
        throw std::runtime_error(
            "cannot flush file; Win32 error " + std::to_string(GetLastError()));
    }
}

void copyFileCreateNew(const std::wstring& source, const std::wstring& destination)
{
    requireRegularFile(source, "copy source");
    if (classifyPath(destination) != PathKind::Missing) {
        throw std::runtime_error("copy destination already exists");
    }
    UniqueHandle in = openExistingFile(source, GENERIC_READ, FILE_SHARE_READ);
    UniqueHandle out = createNewFile(destination);
    try {
        copyHandleToHandle(in.get(), out.get());
    } catch (...) {
        out.reset();
        DeleteFileW(destination.c_str());
        throw;
    }
}

std::string sha256File(const std::wstring& path)
{
    UniqueHandle in = openExistingFile(path, GENERIC_READ, FILE_SHARE_READ);
    CryptProvider provider;
    HCRYPTHASH hash = 0;
    if (!CryptCreateHash(provider.get(), CALG_SHA_256, 0, 0, &hash)) {
        throw std::runtime_error("cannot create SHA-256 hash");
    }
    std::vector<char> buffer(kCopyChunkBytes);
    try {
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(
                    in.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                    &read, nullptr))
            {
                throw std::runtime_error(
                    "cannot read file for hash; Win32 error " +
                    std::to_string(GetLastError()));
            }
            if (read == 0) {
                break;
            }
            if (!CryptHashData(
                    hash, reinterpret_cast<const BYTE*>(buffer.data()), read, 0))
            {
                throw std::runtime_error("cannot hash file");
            }
        }
        BYTE digest[32]{};
        DWORD digestSize = sizeof(digest);
        if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) ||
            digestSize != sizeof(digest))
        {
            throw std::runtime_error("cannot finish SHA-256 hash");
        }
        static const char hex[] = "0123456789abcdef";
        std::string text(64, '0');
        for (int i = 0; i < 32; ++i) {
            text[static_cast<std::size_t>(i) * 2] =
                hex[(digest[i] >> 4) & 0xF];
            text[static_cast<std::size_t>(i) * 2 + 1] =
                hex[digest[i] & 0xF];
        }
        CryptDestroyHash(hash);
        return text;
    } catch (...) {
        CryptDestroyHash(hash);
        throw;
    }
}

std::wstring uniqueStagingPath(const std::wstring& directory)
{
    static volatile LONG sequence = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const LONG value = InterlockedIncrement(&sequence);
        wchar_t name[80]{};
        swprintf_s(
            name,
            L".se-rt-%lu-%lu-%ld",
            GetCurrentProcessId(),
            GetTickCount(),
            value);
        const std::wstring candidate = joinPath(directory, name);
        if (classifyPath(candidate) == PathKind::Missing) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot allocate a unique staging file name");
}

void stageAndAtomicallyReplace(
    const std::wstring& source,
    const std::wstring& destination,
    const std::wstring& stagingDirectory)
{
    requireRegularFile(source, "replacement source");
    switch (classifyPath(destination)) {
    case PathKind::Missing:
    case PathKind::File:
        break;
    case PathKind::Directory:
        throw std::runtime_error("replacement destination is a directory");
    case PathKind::Reparse:
        throw std::runtime_error("replacement destination is a reparse point");
    default:
        throw std::runtime_error("replacement destination is not a regular file");
    }

    const std::wstring staging = uniqueStagingPath(stagingDirectory);
    bool createdStaging = false;
    try {
        UniqueHandle in = openExistingFile(source, GENERIC_READ, FILE_SHARE_READ);
        UniqueHandle out = createNewFile(staging);
        createdStaging = true;
        copyHandleToHandle(in.get(), out.get());
        out.reset();
        in.reset();
        if (!MoveFileExW(
                staging.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error(
                "cannot replace file; Win32 error " +
                std::to_string(GetLastError()));
        }
        createdStaging = false;
    } catch (...) {
        if (createdStaging) {
            DeleteFileW(staging.c_str());
        }
        throw;
    }
}

std::wstring makeTransactionId()
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    wchar_t text[80]{};
    swprintf_s(
        text,
        L"%08lX-%08lX-%08lX-%08lX",
        GetCurrentProcessId(),
        GetTickCount(),
        GetCurrentThreadId(),
        static_cast<unsigned long>(counter.LowPart));
    return text;
}

void validateRollbackLocation(
    const std::wstring& dataDir,
    const std::wstring& rollbackDir)
{
    if (isDriveRoot(dataDir) || isDriveRoot(rollbackDir)) {
        throw std::runtime_error("data-dir and rollback-dir must not be a drive root");
    }
    if (equalPath(dataDir, rollbackDir)) {
        throw std::runtime_error("rollback-dir must not equal data-dir");
    }
    if (isInsideOrEqual(rollbackDir, dataDir)) {
        throw std::runtime_error("rollback-dir must not be inside data-dir");
    }
    const std::wstring dataParent = parentPath(dataDir);
    const std::wstring rollbackParent = parentPath(rollbackDir);
    if (dataParent.empty() || rollbackParent.empty() ||
        !equalPath(dataParent, rollbackParent))
    {
        throw std::runtime_error("rollback-dir parent must match data-dir parent");
    }
    const std::wstring expectedPrefix = fileName(dataDir) + kRuntimeUpdatePrefix;
    const std::wstring rollbackName = fileName(rollbackDir);
    if (rollbackName.size() <= expectedPrefix.size() ||
        CompareStringOrdinal(
            rollbackName.c_str(), static_cast<int>(expectedPrefix.size()),
            expectedPrefix.c_str(), static_cast<int>(expectedPrefix.size()),
            TRUE) != CSTR_EQUAL)
    {
        throw std::runtime_error("rollback-dir basename has an unexpected prefix");
    }
    if (classifyPath(rollbackDir) != PathKind::Missing) {
        throw std::runtime_error("rollback-dir already exists");
    }
}

void createDirectoryExclusive(const std::wstring& path)
{
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        throw std::runtime_error(
            "cannot create rollback directory; Win32 error " +
            std::to_string(GetLastError()));
    }
    if (classifyPath(path) != PathKind::Directory) {
        throw std::runtime_error("rollback directory is not a regular directory");
    }
}

void writeMarker(const std::wstring& rollbackDir)
{
    const std::wstring markerPath = joinPath(rollbackDir, kMarkerName);
    UniqueHandle handle = createNewFile(markerPath);
    const char text[] = "SearchEngineService runtime-update\r\n";
    DWORD written = 0;
    if (!WriteFile(
            handle.get(), text, static_cast<DWORD>(sizeof(text) - 1),
            &written, nullptr) ||
        written != sizeof(text) - 1)
    {
        const DWORD error = GetLastError();
        handle.reset();
        DeleteFileW(markerPath.c_str());
        throw std::runtime_error(
            "cannot write rollback marker; Win32 error " + std::to_string(error));
    }
    if (!FlushFileBuffers(handle.get())) {
        const DWORD error = GetLastError();
        handle.reset();
        DeleteFileW(markerPath.c_str());
        throw std::runtime_error(
            "cannot flush rollback marker; Win32 error " + std::to_string(error));
    }
}

json managedFileToJson(const ManagedFileState& file)
{
    json value = json::object();
    value["existed"] = file.existed;
    if (file.existed) {
        value["snapshot"] = utf8(file.snapshotName);
    }
    return value;
}

void writeManifest(const std::wstring& rollbackDir, const TransactionManifest& manifest)
{
    json root = json::object();
    root["format_version"] = manifest.formatVersion;
    root["transaction_id"] = utf8(manifest.transactionId);
    root["data_dir"] = utf8(manifest.dataDir);
    root["ignore_existed"] = manifest.ignoreExisted;
    root["ignore_sha256"] = manifest.ignoreSha256;
    json files = json::object();
    files["Settings.json"] = managedFileToJson(manifest.settings);
    files["OEM866.INI"] = managedFileToJson(manifest.oem);
    files["client-endpoint.txt"] = managedFileToJson(manifest.endpoint);
    root["managed_files"] = files;

    const std::string body = root.dump(2) + "\n";
    const std::wstring staging = uniqueStagingPath(rollbackDir);
    UniqueHandle handle = createNewFile(staging);
    DWORD written = 0;
    if (!WriteFile(
            handle.get(), body.data(), static_cast<DWORD>(body.size()),
            &written, nullptr) ||
        written != body.size())
    {
        const DWORD error = GetLastError();
        handle.reset();
        DeleteFileW(staging.c_str());
        throw std::runtime_error(
            "cannot write transaction manifest; Win32 error " +
            std::to_string(error));
    }
    if (!FlushFileBuffers(handle.get())) {
        const DWORD error = GetLastError();
        handle.reset();
        DeleteFileW(staging.c_str());
        throw std::runtime_error(
            "cannot flush transaction manifest; Win32 error " +
            std::to_string(error));
    }
    handle.reset();
    const std::wstring manifestPath = joinPath(rollbackDir, kManifestName);
    if (!MoveFileExW(
            staging.c_str(),
            manifestPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        DeleteFileW(staging.c_str());
        throw std::runtime_error(
            "cannot publish transaction manifest; Win32 error " +
            std::to_string(error));
    }
}

ManagedFileState managedFileFromJson(const json& files, const wchar_t* name, const wchar_t* snapshotName)
{
    ManagedFileState state{name, snapshotName};
    const std::string key = utf8(name);
    if (!files.contains(key) || !files[key].is_object()) {
        throw std::runtime_error("transaction manifest is missing a managed file entry");
    }
    const json& entry = files[key];
    if (!entry.contains("existed") || !entry["existed"].is_boolean()) {
        throw std::runtime_error("transaction manifest managed file state is invalid");
    }
    state.existed = entry["existed"].get<bool>();
    if (state.existed) {
        if (!entry.contains("snapshot") || !entry["snapshot"].is_string() ||
            entry["snapshot"].get<std::string>() != utf8(snapshotName))
        {
            throw std::runtime_error("transaction manifest snapshot name is invalid");
        }
    } else if (entry.contains("snapshot")) {
        throw std::runtime_error("transaction manifest snapshot is present for a missing file");
    }
    return state;
}

std::string readAllUtf8(const std::wstring& path)
{
    UniqueHandle handle = openExistingFile(path, GENERIC_READ, FILE_SHARE_READ);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024)
    {
        throw std::runtime_error("transaction manifest is too large or unreadable");
    }
    std::string body(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < body.size()) {
        DWORD read = 0;
        if (!ReadFile(
                handle.get(),
                body.data() + total,
                static_cast<DWORD>(body.size() - total),
                &read,
                nullptr))
        {
            throw std::runtime_error(
                "cannot read transaction manifest; Win32 error " +
                std::to_string(GetLastError()));
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    body.resize(total);
    return body;
}

TransactionManifest loadManifest(const std::wstring& rollbackDir)
{
    const std::wstring markerPath = joinPath(rollbackDir, kMarkerName);
    const std::wstring manifestPath = joinPath(rollbackDir, kManifestName);
    requireRegularFile(markerPath, "transaction marker");
    requireRegularFile(manifestPath, "transaction manifest");

    json root;
    try {
        root = json::parse(readAllUtf8(manifestPath));
    } catch (const std::exception&) {
        throw std::runtime_error("transaction manifest is damaged");
    }
    if (!root.is_object()) {
        throw std::runtime_error("transaction manifest is damaged");
    }
    if (!root.contains("format_version") || !root["format_version"].is_number_integer()) {
        throw std::runtime_error("transaction manifest format_version is missing");
    }
    const int version = root["format_version"].get<int>();
    if (version != kFormatVersion) {
        throw std::runtime_error("unknown transaction manifest format_version");
    }
    TransactionManifest manifest;
    manifest.formatVersion = version;
    if (!root.contains("transaction_id") || !root["transaction_id"].is_string() ||
        root["transaction_id"].get<std::string>().empty())
    {
        throw std::runtime_error("transaction manifest transaction_id is missing");
    }
    manifest.transactionId = utf16(root["transaction_id"].get<std::string>());
    if (!root.contains("data_dir") || !root["data_dir"].is_string() ||
        root["data_dir"].get<std::string>().empty())
    {
        throw std::runtime_error("transaction manifest data_dir is missing");
    }
    manifest.dataDir = utf16(root["data_dir"].get<std::string>());
    if (!root.contains("ignore_existed") || !root["ignore_existed"].is_boolean()) {
        throw std::runtime_error("transaction manifest ignore_existed is missing");
    }
    manifest.ignoreExisted = root["ignore_existed"].get<bool>();
    if (!root.contains("ignore_sha256") || !root["ignore_sha256"].is_string()) {
        throw std::runtime_error("transaction manifest ignore_sha256 is missing");
    }
    manifest.ignoreSha256 = root["ignore_sha256"].get<std::string>();
    if (!root.contains("managed_files") || !root["managed_files"].is_object()) {
        throw std::runtime_error("transaction manifest managed_files is missing");
    }
    const json& files = root["managed_files"];
    manifest.settings = managedFileFromJson(files, kSettingsName, kSettingsSnapshot);
    manifest.oem = managedFileFromJson(files, kOemName, kOemSnapshot);
    manifest.endpoint = managedFileFromJson(files, kEndpointName, kEndpointSnapshot);
    return manifest;
}

void snapshotManagedFile(
    const std::wstring& dataDir,
    const std::wstring& rollbackDir,
    ManagedFileState& file)
{
    const std::wstring source = joinPath(dataDir, file.name);
    switch (classifyPath(source)) {
    case PathKind::Missing:
        file.existed = false;
        return;
    case PathKind::File:
        file.existed = true;
        copyFileCreateNew(source, joinPath(rollbackDir, file.snapshotName));
        return;
    case PathKind::Directory:
        throw std::runtime_error(std::string("managed path is a directory: ") + utf8(file.name));
    case PathKind::Reparse:
        throw std::runtime_error(std::string("managed path is a reparse point: ") + utf8(file.name));
    default:
        throw std::runtime_error(std::string("managed path is invalid: ") + utf8(file.name));
    }
}

void restoreManagedFile(
    const std::wstring& dataDir,
    const std::wstring& rollbackDir,
    const ManagedFileState& file)
{
    const std::wstring destination = joinPath(dataDir, file.name);
    if (file.existed) {
        const std::wstring snapshot = joinPath(rollbackDir, file.snapshotName);
        requireRegularFile(snapshot, "managed file snapshot");
        stageAndAtomicallyReplace(snapshot, destination, dataDir);
        return;
    }
    switch (classifyPath(destination)) {
    case PathKind::Missing:
        return;
    case PathKind::File:
        if (!DeleteFileW(destination.c_str())) {
            throw std::runtime_error(
                "cannot remove installer-created managed file; Win32 error " +
                std::to_string(GetLastError()));
        }
        return;
    case PathKind::Directory:
        throw std::runtime_error("cannot remove managed path because it is a directory");
    case PathKind::Reparse:
        throw std::runtime_error("cannot remove managed path because it is a reparse point");
    default:
        throw std::runtime_error("cannot remove managed path");
    }
}

void restoreIgnore(
    const std::wstring& dataDir,
    const TransactionManifest& manifest)
{
    if (manifest.ignoreExisted) {
        return;
    }
    const std::wstring ignorePath = joinPath(dataDir, kIgnoreName);
    switch (classifyPath(ignorePath)) {
    case PathKind::Missing:
        return;
    case PathKind::File:
        break;
    case PathKind::Directory:
    case PathKind::Reparse:
    default:
        std::cerr << "WARNING: installer-created ignore.txt was not removed "
                     "because ownership cannot be proven\n";
        return;
    }
    std::string actual;
    try {
        actual = sha256File(ignorePath);
    } catch (...) {
        std::cerr << "WARNING: installer-created ignore.txt was not removed "
                     "because ownership cannot be proven\n";
        return;
    }
    if (manifest.ignoreSha256.empty() || actual != manifest.ignoreSha256) {
        std::cerr << "WARNING: installer-created ignore.txt was not removed "
                     "because its contents changed\n";
        return;
    }
    if (!DeleteFileW(ignorePath.c_str())) {
        throw std::runtime_error(
            "cannot remove installer-created ignore.txt; Win32 error " +
            std::to_string(GetLastError()));
    }
}

void ensureDirectory(const std::wstring& path, const char* label)
{
    switch (classifyPath(path)) {
    case PathKind::Directory:
        return;
    case PathKind::Missing:
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error(
                    std::string("cannot create ") + label +
                    "; Win32 error " + std::to_string(error));
            }
            if (classifyPath(path) != PathKind::Directory) {
                throw std::runtime_error(std::string(label) + " is not a directory");
            }
        }
        return;
    case PathKind::File:
        throw std::runtime_error(std::string(label) + " exists as a file");
    case PathKind::Reparse:
        throw std::runtime_error(std::string(label) + " is a reparse point");
    default:
        throw std::runtime_error(std::string(label) + " is invalid");
    }
}

void installIgnoreIfMissing(
    const std::wstring& dataDir,
    const std::wstring& packageIgnore)
{
    const std::wstring ignorePath = joinPath(dataDir, kIgnoreName);
    switch (classifyPath(ignorePath)) {
    case PathKind::File:
        return;
    case PathKind::Directory:
        throw std::runtime_error("ignore.txt is a directory");
    case PathKind::Reparse:
        throw std::runtime_error("ignore.txt is a reparse point");
    case PathKind::Missing:
        break;
    default:
        throw std::runtime_error("ignore.txt is invalid");
    }

    requireRegularFile(packageIgnore, "package ignore.txt");
    UniqueHandle in = openExistingFile(packageIgnore, GENERIC_READ, FILE_SHARE_READ);
    UniqueHandle out;
    const HANDLE created = CreateFileW(
        ignorePath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return;
        }
        throw std::runtime_error(
            "cannot create ignore.txt; Win32 error " + std::to_string(error));
    }
    out = UniqueHandle(created);
    try {
        copyHandleToHandle(in.get(), out.get());
    } catch (...) {
        out.reset();
        DeleteFileW(ignorePath.c_str());
        throw;
    }
}

void restoreAllManaged(
    const std::wstring& dataDir,
    const std::wstring& rollbackDir,
    const TransactionManifest& manifest)
{
    restoreManagedFile(dataDir, rollbackDir, manifest.settings);
    restoreManagedFile(dataDir, rollbackDir, manifest.oem);
    restoreManagedFile(dataDir, rollbackDir, manifest.endpoint);
    restoreIgnore(dataDir, manifest);
}

bool deleteIfRegularFile(const std::wstring& path)
{
    switch (classifyPath(path)) {
    case PathKind::Missing:
        return true;
    case PathKind::File:
        return DeleteFileW(path.c_str()) != 0;
    default:
        return false;
    }
}

struct DirectoryEntry {
    std::wstring name;
    PathKind kind = PathKind::Other;
};

std::vector<DirectoryEntry> listDirectory(const std::wstring& path)
{
    const std::wstring query = joinPath(path, L"*");
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(query.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "cannot enumerate transaction directory; Win32 error " +
            std::to_string(GetLastError()));
    }
    std::vector<DirectoryEntry> entries;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        DirectoryEntry entry;
        entry.name = data.cFileName;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            entry.kind = PathKind::Reparse;
        } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            entry.kind = PathKind::Directory;
        } else {
            entry.kind = PathKind::File;
        }
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    const DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        throw std::runtime_error(
            "cannot enumerate transaction directory; Win32 error " +
            std::to_string(error));
    }
    return entries;
}

std::vector<std::wstring> expectedTransactionFiles(const TransactionManifest& manifest)
{
    std::vector<std::wstring> names;
    names.emplace_back(kMarkerName);
    names.emplace_back(kManifestName);
    if (manifest.settings.existed) {
        names.emplace_back(kSettingsSnapshot);
    }
    if (manifest.oem.existed) {
        names.emplace_back(kOemSnapshot);
    }
    if (manifest.endpoint.existed) {
        names.emplace_back(kEndpointSnapshot);
    }
    return names;
}

bool isExpectedName(const std::vector<std::wstring>& expected, const std::wstring& name)
{
    for (const auto& item : expected) {
        if (equalPath(item, name)) {
            return true;
        }
    }
    return false;
}

void deleteTransactionDirectory(
    const std::wstring& rollbackDir,
    const TransactionManifest& manifest)
{
    requireDirectory(rollbackDir, "rollback-dir");
    const auto expected = expectedTransactionFiles(manifest);
    const auto entries = listDirectory(rollbackDir);
    for (const auto& entry : entries) {
        if (entry.kind != PathKind::File || !isExpectedName(expected, entry.name)) {
            throw std::runtime_error(
                "rollback directory contains unexpected objects and was left in place");
        }
    }
    for (const auto& name : expected) {
        const std::wstring path = joinPath(rollbackDir, name.c_str());
        if (!deleteIfRegularFile(path)) {
            throw std::runtime_error(
                "cannot delete a transaction file; Win32 error " +
                std::to_string(GetLastError()));
        }
    }
    if (!RemoveDirectoryW(rollbackDir.c_str())) {
        throw std::runtime_error(
            "cannot remove empty transaction directory; Win32 error " +
            std::to_string(GetLastError()));
    }
}

void bestEffortDeleteCreatedDirectory(const std::wstring& rollbackDir)
{
    if (classifyPath(rollbackDir) != PathKind::Directory) {
        return;
    }
    try {
        const auto entries = listDirectory(rollbackDir);
        for (const auto& entry : entries) {
            if (entry.kind != PathKind::File) {
                return;
            }
            DeleteFileW(joinPath(rollbackDir, entry.name.c_str()).c_str());
        }
        RemoveDirectoryW(rollbackDir.c_str());
    } catch (...) {
    }
}

void validateExistingTransaction(
    const std::wstring& dataDir,
    const std::wstring& rollbackDir,
    TransactionManifest& manifest)
{
    requireDirectory(rollbackDir, "rollback-dir");
    manifest = loadManifest(rollbackDir);
    const std::wstring manifestDir = canonicalPath(manifest.dataDir);
    if (!equalPath(manifestDir, dataDir)) {
        throw std::runtime_error("transaction manifest data-dir does not match --data-dir");
    }
}

} // namespace

int applyCommand(const std::vector<std::wstring>& args)
{
    const std::wstring dataDir = canonicalPath(requiredOption(args, L"--data-dir"));
    const std::wstring packageData = canonicalPath(requiredOption(args, L"--package-data"));
    const std::wstring generatedSettings =
        canonicalPath(requiredOption(args, L"--generated-settings"));
    const std::wstring generatedEndpoint =
        canonicalPath(requiredOption(args, L"--generated-endpoint"));
    const std::wstring rollbackDir = canonicalPath(requiredOption(args, L"--rollback-dir"));

    requireDirectory(dataDir, "data-dir");
    requireDirectory(packageData, "package-data");
    requireRegularFile(generatedSettings, "generated Settings.json");
    requireRegularFile(generatedEndpoint, "generated client-endpoint.txt");
    const std::wstring packageOem = joinPath(packageData, kOemName);
    const std::wstring packageIgnore = joinPath(packageData, kIgnoreName);
    requireRegularFile(packageOem, "package OEM866.INI");
    requireRegularFile(packageIgnore, "package ignore.txt");
    validateRollbackLocation(dataDir, rollbackDir);

    bool createdRollback = false;
    bool mutated = false;
    TransactionManifest manifest;
    try {
        createDirectoryExclusive(rollbackDir);
        createdRollback = true;
        writeMarker(rollbackDir);

        manifest.formatVersion = kFormatVersion;
        manifest.transactionId = makeTransactionId();
        manifest.dataDir = dataDir;
        snapshotManagedFile(dataDir, rollbackDir, manifest.settings);
        snapshotManagedFile(dataDir, rollbackDir, manifest.oem);
        snapshotManagedFile(dataDir, rollbackDir, manifest.endpoint);

        const std::wstring ignorePath = joinPath(dataDir, kIgnoreName);
        switch (classifyPath(ignorePath)) {
        case PathKind::Missing:
            manifest.ignoreExisted = false;
            manifest.ignoreSha256 = sha256File(packageIgnore);
            break;
        case PathKind::File:
            manifest.ignoreExisted = true;
            break;
        case PathKind::Directory:
            throw std::runtime_error("ignore.txt is a directory");
        case PathKind::Reparse:
            throw std::runtime_error("ignore.txt is a reparse point");
        default:
            throw std::runtime_error("ignore.txt is invalid");
        }
        writeManifest(rollbackDir, manifest);

        stageAndAtomicallyReplace(generatedSettings, joinPath(dataDir, kSettingsName), dataDir);
        mutated = true;
        stageAndAtomicallyReplace(packageOem, joinPath(dataDir, kOemName), dataDir);
        stageAndAtomicallyReplace(generatedEndpoint, joinPath(dataDir, kEndpointName), dataDir);
        installIgnoreIfMissing(dataDir, packageIgnore);
        ensureDirectory(joinPath(dataDir, kLogsName), "logs");
        ensureDirectory(joinPath(dataDir, kMessagesName), "messages");
        return 0;
    } catch (const std::exception& exception) {
        if (mutated) {
            try {
                restoreAllManaged(dataDir, rollbackDir, manifest);
                deleteTransactionDirectory(rollbackDir, manifest);
                std::cerr << "ERROR: " << exception.what() << '\n';
                std::cerr << "runtime-update apply failed; managed files were rolled back\n";
                return 1;
            } catch (const std::exception& rollbackError) {
                std::cerr << "ERROR: " << exception.what() << '\n';
                std::cerr << "ERROR: runtime-update rollback incomplete: "
                          << rollbackError.what() << '\n';
                std::cerr << "rollback_dir=" << utf8(rollbackDir) << '\n';
                return 1;
            }
        }
        if (createdRollback) {
            bestEffortDeleteCreatedDirectory(rollbackDir);
        }
        std::cerr << "ERROR: " << exception.what() << '\n';
        return 1;
    }
}

int rollbackCommand(const std::vector<std::wstring>& args)
{
    try {
        const std::wstring dataDir = canonicalPath(requiredOption(args, L"--data-dir"));
        const std::wstring rollbackDir = canonicalPath(requiredOption(args, L"--rollback-dir"));
        requireDirectory(dataDir, "data-dir");
        if (isDriveRoot(dataDir) || isDriveRoot(rollbackDir)) {
            throw std::runtime_error("data-dir and rollback-dir must not be a drive root");
        }
        TransactionManifest manifest;
        validateExistingTransaction(dataDir, rollbackDir, manifest);
        restoreAllManaged(dataDir, rollbackDir, manifest);
        deleteTransactionDirectory(rollbackDir, manifest);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ERROR: " << exception.what() << '\n';
        if (const auto rollback = option(args, L"--rollback-dir")) {
            try {
                std::cerr << "rollback_dir=" << utf8(canonicalPath(*rollback)) << '\n';
            } catch (...) {
                std::cerr << "rollback_dir=" << utf8(*rollback) << '\n';
            }
        }
        return 1;
    }
}

int commitCommand(const std::vector<std::wstring>& args)
{
    try {
        const std::wstring dataDir = canonicalPath(requiredOption(args, L"--data-dir"));
        const std::wstring rollbackDir = canonicalPath(requiredOption(args, L"--rollback-dir"));
        requireDirectory(dataDir, "data-dir");
        if (isDriveRoot(dataDir) || isDriveRoot(rollbackDir)) {
            throw std::runtime_error("data-dir and rollback-dir must not be a drive root");
        }
        TransactionManifest manifest;
        validateExistingTransaction(dataDir, rollbackDir, manifest);
        deleteTransactionDirectory(rollbackDir, manifest);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ERROR: " << exception.what() << '\n';
        if (const auto rollback = option(args, L"--rollback-dir")) {
            try {
                std::cerr << "rollback_dir=" << utf8(canonicalPath(*rollback)) << '\n';
            } catch (...) {
                std::cerr << "rollback_dir=" << utf8(*rollback) << '\n';
            }
        }
        return 1;
    }
}

} // namespace runtime_data_transaction
