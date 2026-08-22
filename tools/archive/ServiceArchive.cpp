#include "ServiceArchive.h"

#include "Backup/FileHash.h"
#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace searchengine_archive {
namespace {

using json = nlohmann::json;

constexpr wchar_t kManifestName[] = L"archive-operation.json";
constexpr DWORD kServiceWaitMilliseconds = 120000;

std::string utf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

std::string utf8(const std::wstring& value)
{
    return encoding::wstring_to_utf8(value);
}

fs::path fromUtf8(const std::string& value)
{
    return fs::path(encoding::utf8_to_wstring(value));
}

std::wstring lower(std::wstring value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool samePath(const fs::path& left, const fs::path& right)
{
    return isPathEqualOrBelow(left, right) && isPathEqualOrBelow(right, left);
}

bool pathsOverlap(const fs::path& left, const fs::path& right)
{
    return isPathEqualOrBelow(left, right) || isPathEqualOrBelow(right, left);
}

bool isDriveRoot(const fs::path& value)
{
    const fs::path normalized = value.lexically_normal();
    return !normalized.root_path().empty() &&
        normalized == normalized.root_path();
}

fs::path absoluteNormalized(const fs::path& value)
{
    std::error_code error;
    fs::path result = fs::absolute(value, error);
    if (error)
        result = value;
    return result.lexically_normal();
}

std::wstring expandEnvironment(const std::wstring& value)
{
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0)
        throw std::runtime_error("ExpandEnvironmentStringsW failed");
    std::wstring result(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        value.c_str(), result.data(), required);
    if (written == 0 || written > required)
        throw std::runtime_error("ExpandEnvironmentStringsW failed");
    result.resize(written - 1);
    return result;
}

std::wstring quoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open JSON: " + utf8(path));
    json value;
    input >> value;
    if (!input.eof() && input.fail())
        throw std::runtime_error("cannot parse JSON: " + utf8(path));
    return value;
}

void saveJsonAtomically(const fs::path& path, const json& value)
{
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot write JSON: " + utf8(temporary));
        output << value.dump(2) << '\n';
        if (!output)
            throw std::runtime_error("cannot finish JSON: " + utf8(temporary));
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        throw std::runtime_error(
            "cannot publish JSON; Win32 error=" +
            std::to_string(GetLastError()));
    }
}

class ServiceHandle final {
public:
    explicit ServiceHandle(SC_HANDLE handle = nullptr) : handle_(handle) {}
    ~ServiceHandle()
    {
        if (handle_)
            CloseServiceHandle(handle_);
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    ServiceHandle(ServiceHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    ServiceHandle& operator=(ServiceHandle&& other) noexcept
    {
        if (this != &other) {
            if (handle_)
                CloseServiceHandle(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    SC_HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    SC_HANDLE handle_{};
};

class ScManager final {
public:
    explicit ScManager(DWORD access)
        : handle_(OpenSCManagerW(nullptr, nullptr, access))
    {
        if (!handle_)
            throw std::runtime_error(
                "cannot open Windows Service Control Manager; Win32 error=" +
                std::to_string(GetLastError()));
    }
    SC_HANDLE get() const noexcept { return handle_.get(); }

private:
    ServiceHandle handle_;
};

SERVICE_STATUS_PROCESS queryStatus(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(
            service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes))
    {
        throw std::runtime_error(
            "QueryServiceStatusEx failed; Win32 error=" +
            std::to_string(GetLastError()));
    }
    return status;
}

std::wstring queryImagePath(SC_HANDLE service)
{
    DWORD bytes = 0;
    QueryServiceConfigW(service, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        throw std::runtime_error("QueryServiceConfigW size query failed");
    std::vector<BYTE> buffer(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, bytes, &bytes))
        throw std::runtime_error(
            "QueryServiceConfigW failed; Win32 error=" +
            std::to_string(GetLastError()));
    return config->lpBinaryPathName ? config->lpBinaryPathName : L"";
}

void waitForState(SC_HANDLE service, DWORD desired)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kServiceWaitMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = queryStatus(service);
        if (status.dwCurrentState == desired)
            return;
        if (desired == SERVICE_RUNNING &&
            status.dwCurrentState == SERVICE_STOPPED)
        {
            throw std::runtime_error(
                "service stopped during startup; service Win32 exit=" +
                std::to_string(status.dwWin32ExitCode));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error("timeout waiting for Windows service state");
}

void stopService(SC_HANDLE service)
{
    auto status = queryStatus(service);
    if (status.dwCurrentState == SERVICE_STOPPED)
        return;
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        waitForState(service, SERVICE_STOPPED);
        return;
    }
    if (status.dwCurrentState != SERVICE_RUNNING &&
        status.dwCurrentState != SERVICE_PAUSED)
    {
        throw std::runtime_error("service is in an unsupported transition");
    }
    SERVICE_STATUS control{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control))
        throw std::runtime_error(
            "ControlService(STOP) failed; Win32 error=" +
            std::to_string(GetLastError()));
    waitForState(service, SERVICE_STOPPED);
}

void startService(SC_HANDLE service)
{
    const auto status = queryStatus(service);
    if (status.dwCurrentState == SERVICE_RUNNING)
        return;
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        waitForState(service, SERVICE_RUNNING);
        return;
    }
    if (status.dwCurrentState != SERVICE_STOPPED)
        throw std::runtime_error("service is not stopped before startup");
    if (!StartServiceW(service, 0, nullptr))
        throw std::runtime_error(
            "StartServiceW failed; Win32 error=" +
            std::to_string(GetLastError()));
    waitForState(service, SERVICE_RUNNING);
}

void setImagePath(SC_HANDLE service, const std::wstring& imagePath)
{
    if (!ChangeServiceConfigW(
            service,
            SERVICE_NO_CHANGE,
            SERVICE_NO_CHANGE,
            SERVICE_NO_CHANGE,
            imagePath.c_str(),
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
    {
        throw std::runtime_error(
            "ChangeServiceConfigW failed; Win32 error=" +
            std::to_string(GetLastError()));
    }
    if (queryImagePath(service) != imagePath)
        throw std::runtime_error("SCM ImagePath verification failed");
}

ServiceHandle openServiceForArchive(
    SC_HANDLE manager,
    const std::wstring& serviceName)
{
    ServiceHandle service(OpenServiceW(
        manager,
        serviceName.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
        SERVICE_STOP | SERVICE_START | SERVICE_CHANGE_CONFIG));
    if (!service)
        throw std::runtime_error(
            "cannot open service '" + utf8(serviceName) +
            "'; Win32 error=" + std::to_string(GetLastError()));
    return service;
}

struct CopiedFile {
    fs::path source;
    fs::path stagedTarget;
    FileHashResult original;
};

bool isTlgRoot(const fs::path& value)
{
    return samePath(value, fs::path(L"D:\\TLG"));
}

bool isReparsePoint(const fs::path& value)
{
    const DWORD attributes = GetFileAttributesW(value.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void copyOneFile(
    const fs::path& source,
    const fs::path& target,
    std::vector<CopiedFile>& copied)
{
    fs::create_directories(target.parent_path());
    if (fs::exists(target))
        throw std::runtime_error("target collision: " + utf8(target));
    std::error_code error;
    fs::copy_file(source, target, fs::copy_options::none, error);
    if (error)
        throw std::runtime_error(
            "cannot copy '" + utf8(source) + "': " + error.message());
    const auto sourceTime = fs::last_write_time(source, error);
    if (!error)
        fs::last_write_time(target, sourceTime, error);
    const FileHashResult sourceHash = sha256File(source);
    const FileHashResult targetHash = sha256File(target);
    if (!sourceHash.ok || !targetHash.ok ||
        sourceHash.size != targetHash.size ||
        sourceHash.sha256 != targetHash.sha256)
    {
        throw std::runtime_error(
            "copied file verification failed: " + utf8(source));
    }
    copied.push_back({source, target, sourceHash});
}

void copyTree(
    const fs::path& source,
    const fs::path& target,
    int archivedYear,
    std::vector<CopiedFile>& copied,
    const std::vector<fs::path>& excludedRoots = {})
{
    if (!fs::is_directory(source))
        throw std::runtime_error("source directory is missing: " + utf8(source));
    if (isReparsePoint(source))
        throw std::runtime_error("source root is a reparse point: " + utf8(source));
    fs::create_directories(target);
    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
        const bool excluded = std::any_of(
            excludedRoots.begin(), excludedRoots.end(),
            [&](const fs::path& root) {
                return !root.empty() && isPathEqualOrBelow(it->path(), root);
            });
        if (excluded) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (isReparsePoint(it->path())) {
            if (it->is_directory())
                it.disable_recursion_pending();
            throw std::runtime_error(
                "reparse points are not archived automatically: " +
                utf8(it->path()));
        }
        const fs::path relative = it->path().lexically_relative(source);
        if (shouldSkipArchiveTreeEntry(
                source,
                it->path().filename(),
                it.depth(),
                it->is_directory(),
                archivedYear))
        {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        const fs::path destination = target / relative;
        if (it->is_directory())
            fs::create_directories(destination);
        else if (it->is_regular_file())
            copyOneFile(it->path(), destination, copied);
    }
}

std::wstring safeServiceLeaf(const std::wstring& serviceName)
{
    std::wstring value = serviceName;
    for (wchar_t& ch : value) {
        if (!std::iswalnum(ch) && ch != L'-' && ch != L'_')
            ch = L'_';
    }
    return value.empty() ? L"SearchEngineService" : value;
}

void addMapping(
    ServiceArchivePlan& plan,
    const fs::path& source,
    const fs::path& target)
{
    const fs::path normalized = absoluteNormalized(source);
    if (normalized.empty() || !normalized.is_absolute() || isDriveRoot(normalized))
        throw std::runtime_error("unsafe source root: " + utf8(source));
    for (const auto& existing : plan.mappings) {
        if (samePath(existing.target, target) &&
            !samePath(existing.source, normalized))
        {
            throw std::runtime_error(
                "archive content roots have the same folder name; "
                "rename one source folder: " + utf8(normalized.filename()));
        }
        if (isPathEqualOrBelow(normalized, existing.source))
            return;
        if (isPathEqualOrBelow(existing.source, normalized)) {
            throw std::runtime_error(
                "archive content root contains a server program/data root: " +
                utf8(normalized));
        }
    }
    if (isPathEqualOrBelow(plan.finalDirectory, normalized) ||
        isPathEqualOrBelow(normalized, plan.finalDirectory))
    {
        throw std::runtime_error(
            "archive destination overlaps a source root: " + utf8(normalized));
    }
    plan.mappings.push_back({normalized, target});
}

std::string requiredString(const json& config, const char* name)
{
    if (!config.contains(name) || !config.at(name).is_string())
        throw std::runtime_error(std::string("config.") + name + " is required");
    return config.at(name).get<std::string>();
}

std::vector<fs::path> configuredIndexRoots(const json& config)
{
    if (!config.contains("index_roots") ||
        !config.at("index_roots").is_array() ||
        config.at("index_roots").empty())
    {
        throw std::runtime_error("config.index_roots must be a non-empty array");
    }
    std::vector<fs::path> roots;
    for (const auto& value : config.at("index_roots")) {
        if (!value.is_string())
            throw std::runtime_error("config.index_roots contains a non-string");
        roots.push_back(fromUtf8(value.get<std::string>()));
    }
    return roots;
}

fs::path configuredMonthlyDirectory(
    const json& config,
    const char* explicitName,
    const char* baseName)
{
    if (config.contains(explicitName)) {
        if (!config.at(explicitName).is_string())
            throw std::runtime_error(
                std::string("config.") + explicitName + " must be a string");
        return fromUtf8(config.at(explicitName).get<std::string>());
    }
    const std::string base = requiredString(config, baseName);
    return base.empty() ? fs::path{} : fromUtf8(base) / L"METH_BASES";
}

std::optional<fs::path> tryRebase(
    const fs::path& value,
    const std::vector<PathMapping>& mappings)
{
    for (const auto& mapping : mappings) {
        if (isPathEqualOrBelow(value, mapping.source))
            return rebasePath(value, mappings);
    }
    return std::nullopt;
}

void rewriteSettings(
    const fs::path& settingsPath,
    const std::vector<PathMapping>& mappings,
    const fs::path& prmMonthlyDirectory,
    const fs::path& prdMonthlyDirectory,
    const char* mode)
{
    json root = readJson(settingsPath);
    if (!root.contains("config") || !root.at("config").is_object())
        throw std::runtime_error("Settings.json has no config object");
    json& config = root["config"];
    const auto rewriteArray = [&](const char* name, bool keepOutside) {
        if (!config.contains(name))
            return;
        if (!config.at(name).is_array())
            throw std::runtime_error(std::string("config.") + name + " must be an array");
        for (auto& item : config[name]) {
            if (!item.is_string())
                throw std::runtime_error(std::string("config.") + name + " contains a non-string");
            const fs::path source = fromUtf8(item.get<std::string>());
            if (const auto target = tryRebase(source, mappings))
                item = utf8(*target);
            else if (!keepOutside)
                throw std::runtime_error(
                    std::string("config.") + name +
                    " path is outside archive mappings: " + utf8(source));
        }
    };
    rewriteArray("index_roots", false);
    rewriteArray("excluded_subtrees", true);
    rewriteArray("exclude_dirs", true);

    config["server_mode"] = mode;
    config["document_catalog_storage"] = "sqlite";
    config["scan_on_startup"] = false;
    config["prm_monthly_bases_dir"] = utf8(prmMonthlyDirectory);
    config["prd_monthly_bases_dir"] = utf8(prdMonthlyDirectory);
    if (std::string(mode) == "archive") {
        // ARCHIVE.db3 is the live operational database.  A frozen server uses
        // only the copied monthly databases and must have no writable base
        // directory in which SQLite could recreate ARCHIVE.db3.
        config["prm_base_dir"] = "";
        config["prd_base_dir"] = "";
    } else {
        config["prm_base_dir"] = prmMonthlyDirectory.empty()
            ? std::string() : utf8(prmMonthlyDirectory.parent_path());
        config["prd_base_dir"] = prdMonthlyDirectory.empty()
            ? std::string() : utf8(prdMonthlyDirectory.parent_path());
    }
    saveJsonAtomically(settingsPath, root);
}

void ensureDocsSchema(sqlite3* database)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database, "PRAGMA table_info(docs)", -1, &statement, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("cannot inspect inverted_index.sqlite docs schema");
    }
    bool path = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* raw = sqlite3_column_text(statement, 1);
        path = path || (raw && std::string(
            reinterpret_cast<const char*>(raw)) == "path");
    }
    sqlite3_finalize(statement);
    if (!path)
        throw std::runtime_error("inverted_index.sqlite has no docs.path column");
}

void sqliteExec(sqlite3* database, const char* sql)
{
    char* rawError = nullptr;
    const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
    if (rc == SQLITE_OK)
        return;
    const std::string detail = rawError ? rawError : sqlite3_errmsg(database);
    sqlite3_free(rawError);
    throw std::runtime_error("SQLite command failed: " + detail);
}

json mappingJson(const std::vector<PathMapping>& mappings)
{
    json value = json::array();
    for (const auto& mapping : mappings) {
        value.push_back({
            {"source", utf8(mapping.source)},
            {"target", utf8(mapping.target)}});
    }
    return value;
}

std::vector<PathMapping> mappingsFromJson(const json& value, bool reverse)
{
    std::vector<PathMapping> result;
    for (const auto& item : value) {
        const fs::path source = fromUtf8(item.at("source").get<std::string>());
        const fs::path target = fromUtf8(item.at("target").get<std::string>());
        result.push_back(reverse
            ? PathMapping{target, source}
            : PathMapping{source, target});
    }
    return result;
}

void ensureNoOtherServiceUsesSources(
    const std::wstring& selectedService,
    const std::vector<PathMapping>& mappings)
{
    for (const auto& other : enumerateSearchEngineServices()) {
        if (other.serviceName == selectedService)
            continue;
        const std::vector<fs::path> otherRuntimeRoots{
            other.executable.parent_path(), other.dataDirectory};
        for (const auto& mapping : mappings) {
            for (const auto& otherRoot : otherRuntimeRoots) {
                if (pathsOverlap(mapping.source, otherRoot)) {
                    throw std::runtime_error(
                        "source root is also used by service '" +
                        utf8(other.serviceName) + "': " + utf8(mapping.source));
                }
            }
        }
    }
}

void removeEmptyDirectoryTree(const fs::path& root)
{
    if (!fs::exists(root))
        return;
    if (!fs::is_directory(root) || isDriveRoot(root) || isReparsePoint(root))
        throw std::runtime_error("restore target is not a safe directory: " + utf8(root));
    std::vector<fs::path> directories;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (isReparsePoint(it->path()) || !it->is_directory()) {
            throw std::runtime_error(
                "restore target contains a file or reparse point: " +
                utf8(it->path()));
        }
        directories.push_back(it->path());
    }
    std::sort(
        directories.begin(), directories.end(),
        [](const fs::path& left, const fs::path& right) {
            return left.wstring().size() > right.wstring().size();
        });
    for (const auto& directory : directories) {
        std::error_code error;
        if (!fs::remove(directory, error) || error) {
            throw std::runtime_error(
                "cannot remove empty restore directory: " + utf8(directory));
        }
    }
    std::error_code error;
    if (!fs::remove(root, error) || error)
        throw std::runtime_error("cannot remove empty restore root: " + utf8(root));
}

void publishMergedTree(const fs::path& staging, const fs::path& target)
{
    fs::create_directories(target);
    for (fs::recursive_directory_iterator it(staging), end; it != end; ++it) {
        const fs::path relative = it->path().lexically_relative(staging);
        const fs::path destination = target / relative;
        if (it->is_directory()) {
            fs::create_directories(destination);
        } else if (it->is_regular_file()) {
            if (fs::exists(destination)) {
                const FileHashResult stagedHash = sha256File(it->path());
                const FileHashResult targetHash = sha256File(destination);
                if (!stagedHash.ok || !targetHash.ok ||
                    stagedHash.size != targetHash.size ||
                    stagedHash.sha256 != targetHash.sha256)
                {
                    throw std::runtime_error(
                        "restore merge target already differs: " +
                        utf8(destination));
                }
                continue;
            }
            std::error_code error;
            fs::rename(it->path(), destination, error);
            if (error) {
                throw std::runtime_error(
                    "cannot publish restored file: " + error.message());
            }
        } else {
            throw std::runtime_error(
                "unsupported entry in restore staging: " + utf8(it->path()));
        }
    }
}

bool directoryTreesEqual(const fs::path& left, const fs::path& right)
{
    if (!fs::is_directory(left) || !fs::is_directory(right))
        return false;
    const auto files = [](const fs::path& root) {
        std::vector<std::pair<fs::path, FileHashResult>> result;
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            if (isReparsePoint(it->path()))
                throw std::runtime_error("reparse point in restore tree");
            if (it->is_regular_file())
                result.emplace_back(
                    it->path().lexically_relative(root), sha256File(it->path()));
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return lower(a.first.wstring()) < lower(b.first.wstring());
        });
        return result;
    };
    const auto leftFiles = files(left);
    const auto rightFiles = files(right);
    if (leftFiles.size() != rightFiles.size())
        return false;
    for (std::size_t index = 0; index < leftFiles.size(); ++index) {
        if (!samePath(leftFiles[index].first, rightFiles[index].first) ||
            !leftFiles[index].second.ok || !rightFiles[index].second.ok ||
            leftFiles[index].second.size != rightFiles[index].second.size ||
            leftFiles[index].second.sha256 != rightFiles[index].second.sha256)
        {
            return false;
        }
    }
    return true;
}

bool isStrictlyBelow(const fs::path& value, const fs::path& root)
{
    return isPathEqualOrBelow(value, root) && !samePath(value, root);
}

void ensureServicesDoNotUseArchive(
    const fs::path& archiveDirectory,
    const std::vector<InstalledService>& services)
{
    for (const auto& service : services) {
        if (pathsOverlap(service.executable.parent_path(), archiveDirectory) ||
            pathsOverlap(service.dataDirectory, archiveDirectory))
        {
            throw std::runtime_error(
                "SearchEngine service still uses the archive directory: " +
                utf8(service.serviceName));
        }
    }
}

void ensureArchiveTreeHasNoReparsePoints(const fs::path& archiveDirectory)
{
    if (isReparsePoint(archiveDirectory))
        throw std::runtime_error("archive directory is a reparse point");
    for (fs::recursive_directory_iterator it(archiveDirectory), end; it != end; ++it) {
        if (isReparsePoint(it->path())) {
            if (it->is_directory())
                it.disable_recursion_pending();
            throw std::runtime_error(
                "archive contains a reparse point: " + utf8(it->path()));
        }
    }
}

} // namespace

ServiceInvocation parseServiceInvocation(const std::wstring& imagePath)
{
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(imagePath.c_str(), &count);
    if (!raw || count < 1) {
        if (raw)
            LocalFree(raw);
        throw std::runtime_error("cannot parse service ImagePath");
    }
    struct ArgsGuard {
        LPWSTR* value;
        ~ArgsGuard() { LocalFree(value); }
    } guard{raw};

    ServiceInvocation invocation;
    invocation.imagePath = imagePath;
    invocation.executable = absoluteNormalized(expandEnvironment(raw[0]));
    bool serviceFlag = false;
    std::optional<std::wstring> data;
    for (int index = 1; index < count; ++index) {
        const std::wstring argument = raw[index];
        if (argument == L"--service") {
            serviceFlag = true;
        } else if (argument == L"--service-name" && index + 1 < count) {
            invocation.serviceNameArgument = raw[++index];
        } else if ((argument == L"--data-dir" || argument == L"--base-dir") &&
                   index + 1 < count) {
            if (data)
                throw std::runtime_error("service ImagePath has duplicate data-dir");
            data = expandEnvironment(raw[++index]);
        }
    }
    if (!serviceFlag)
        throw std::runtime_error("service ImagePath has no --service flag");
    if (!data || data->empty())
        throw std::runtime_error("service ImagePath has no --data-dir");
    invocation.dataDirectory = fs::path(*data).is_absolute()
        ? absoluteNormalized(*data)
        : absoluteNormalized(invocation.executable.parent_path() / *data);
    return invocation;
}

std::wstring buildServiceImagePath(
    const fs::path& executable,
    const std::wstring& serviceName,
    const fs::path& dataDirectory)
{
    if (serviceName.empty())
        throw std::runtime_error("service name is empty");
    return quoteArgument(absoluteNormalized(executable).wstring()) +
        L" --service --service-name " + quoteArgument(serviceName) +
        L" --data-dir " + quoteArgument(
            absoluteNormalized(dataDirectory).wstring());
}

std::vector<InstalledService> enumerateSearchEngineServices()
{
    ScManager manager(SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    DWORD bytes = 0;
    DWORD count = 0;
    DWORD resume = 0;
    EnumServicesStatusExW(
        manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
        SERVICE_STATE_ALL, nullptr, 0, &bytes, &count, &resume, nullptr);
    if (GetLastError() != ERROR_MORE_DATA)
        return {};
    std::vector<BYTE> buffer(bytes);
    resume = 0;
    if (!EnumServicesStatusExW(
            manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
            SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()),
            &bytes, &count, &resume, nullptr))
    {
        throw std::runtime_error("cannot enumerate Windows services");
    }
    const auto* services =
        reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    std::vector<InstalledService> result;
    for (DWORD index = 0; index < count; ++index) {
        const std::wstring name = services[index].lpServiceName;
        if (name != L"SearchEngineService" &&
            name.rfind(L"SearchEngineService-", 0) != 0)
        {
            continue;
        }
        try {
            result.push_back(inspectSearchEngineService(name));
        } catch (...) {
            // A similarly named but malformed service is not an eligible target.
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.serviceName < right.serviceName;
    });
    return result;
}

InstalledService inspectSearchEngineService(const std::wstring& serviceName)
{
    ScManager manager(SC_MANAGER_CONNECT);
    ServiceHandle service(OpenServiceW(
        manager.get(), serviceName.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service)
        throw std::runtime_error(
            "SearchEngine service was not found: " + utf8(serviceName));
    const std::wstring imagePath = queryImagePath(service.get());
    const ServiceInvocation invocation = parseServiceInvocation(imagePath);
    if (!invocation.serviceNameArgument.empty() &&
        invocation.serviceNameArgument != serviceName)
    {
        throw std::runtime_error("SCM name and --service-name do not match");
    }

    DWORD bytes = 0;
    QueryServiceConfigW(service.get(), nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service.get(), config, bytes, &bytes))
        throw std::runtime_error("QueryServiceConfigW failed");

    InstalledService result;
    result.serviceName = serviceName;
    result.displayName = config->lpDisplayName ? config->lpDisplayName : L"";
    result.imagePath = imagePath;
    result.executable = invocation.executable;
    result.dataDirectory = invocation.dataDirectory;
    result.currentState = queryStatus(service.get()).dwCurrentState;
    return result;
}

ServiceArchivePlan planServiceArchive(const ServiceArchiveOptions& options)
{
    if (options.serviceName.empty())
        throw std::runtime_error("service name is required");
    if (options.archiveRoot.empty() || !options.archiveRoot.is_absolute() ||
        isDriveRoot(options.archiveRoot))
    {
        throw std::runtime_error("archive root must be a safe absolute directory");
    }

    ServiceArchivePlan plan;
    plan.options = options;
    plan.options.archiveRoot = absoluteNormalized(options.archiveRoot);
    plan.service = inspectSearchEngineService(options.serviceName);
    if (!fs::is_regular_file(plan.service.executable))
        throw std::runtime_error("service executable is missing");
    if (!fs::is_directory(plan.service.dataDirectory))
        throw std::runtime_error("service data directory is missing");

    const fs::path settingsPath =
        plan.service.dataDirectory / L"Settings.json";
    const json settings = readJson(settingsPath);
    if (!settings.contains("config") || !settings.at("config").is_object())
        throw std::runtime_error("Settings.json has no config object");
    const json& config = settings.at("config");
    if (config.value("document_catalog_storage", std::string("memory")) !=
        "sqlite")
    {
        throw std::runtime_error(
            "service archive requires document_catalog_storage=sqlite");
    }
    const std::string yearText = requiredString(config, "year");
    plan.year = std::stoi(yearText);
    if (plan.year < 1900 || plan.year > 9999)
        throw std::runtime_error("configured year is outside 1900..9999");

    plan.finalDirectory = plan.options.archiveRoot /
        (safeServiceLeaf(options.serviceName) + L"-" +
         std::to_wstring(plan.year));
    if (fs::exists(plan.finalDirectory))
        throw std::runtime_error(
            "service archive directory already exists: " +
            utf8(plan.finalDirectory));

    addMapping(
        plan,
        plan.service.executable.parent_path(),
        plan.finalDirectory / L"program");
    addMapping(
        plan,
        plan.service.dataDirectory,
        plan.finalDirectory / L"data");

    YearMoveOptions yearOptions;
    yearOptions.year = plan.year;
    yearOptions.prmMonthlyDirectory = configuredMonthlyDirectory(
        config, "prm_monthly_bases_dir", "prm_base_dir");
    yearOptions.prdMonthlyDirectory = configuredMonthlyDirectory(
        config, "prd_monthly_bases_dir", "prd_base_dir");
    plan.originalPrmMonthlyDirectory = yearOptions.prmMonthlyDirectory;
    plan.originalPrdMonthlyDirectory = yearOptions.prdMonthlyDirectory;
    yearOptions.archiveRoot = plan.finalDirectory;
    if (!yearOptions.prmMonthlyDirectory.empty() ||
        !yearOptions.prdMonthlyDirectory.empty())
    {
        plan.monthlyDatabases = inspectMonthlyDatabases(
            yearOptions, &plan.warnings);
    }

    std::vector<fs::path> contentRoots = configuredIndexRoots(config);
    for (const auto& database : plan.monthlyDatabases) {
        std::string verifyError;
        if (!verifySQLiteDatabase(database.source, verifyError)) {
            throw std::runtime_error(
                "monthly SQLite integrity check failed: " + verifyError);
        }
        auto roots = inspectAutoPadDirectToRoots(database.source);
        contentRoots.insert(contentRoots.end(), roots.begin(), roots.end());
    }
    contentRoots = collapseSourceRoots(std::move(contentRoots));
    for (const auto& root : contentRoots) {
        if (!fs::is_directory(root))
            throw std::runtime_error("archive content root is missing: " + utf8(root));
        for (const auto& monthlyDirectory : {
                 plan.originalPrmMonthlyDirectory,
                 plan.originalPrdMonthlyDirectory})
        {
            if (!monthlyDirectory.empty() &&
                (isPathEqualOrBelow(root, monthlyDirectory) ||
                 isPathEqualOrBelow(monthlyDirectory, root)))
            {
                throw std::runtime_error(
                    "indexed content and monthly database directories overlap: " +
                    utf8(root));
            }
        }
        addMapping(
            plan,
            root,
            plan.finalDirectory / L"content" /
                safeServiceLeaf(root.filename().wstring()));
    }

    plan.archivedExecutable = rebasePath(
        plan.service.executable, plan.mappings);
    plan.archivedDataDirectory = rebasePath(
        plan.service.dataDirectory, plan.mappings);
    plan.archivedImagePath = buildServiceImagePath(
        plan.archivedExecutable,
        plan.service.serviceName,
        plan.archivedDataDirectory);
    ensureNoOtherServiceUsesSources(plan.service.serviceName, plan.mappings);
    return plan;
}

void rewriteDocumentCatalogPaths(
    const fs::path& database,
    const std::vector<PathMapping>& mappings)
{
    sqlite3* handle = nullptr;
    const std::string databaseUtf8 = utf8(database);
    if (sqlite3_open_v2(
            databaseUtf8.c_str(), &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        const std::string detail = handle ? sqlite3_errmsg(handle) : "no handle";
        if (handle)
            sqlite3_close_v2(handle);
        throw std::runtime_error("cannot open inverted index: " + detail);
    }
    struct DbGuard {
        sqlite3* value;
        ~DbGuard() { sqlite3_close_v2(value); }
    } guard{handle};
    sqlite3_busy_timeout(handle, 30000);
    ensureDocsSchema(handle);
    sqliteExec(handle, "BEGIN IMMEDIATE");
    sqlite3_stmt* select = nullptr;
    sqlite3_stmt* update = nullptr;
    try {
        if (sqlite3_prepare_v2(
                handle, "SELECT doc_id,path FROM docs ORDER BY doc_id",
                -1, &select, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(
                handle, "UPDATE docs SET path=? WHERE doc_id=?",
                -1, &update, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error("cannot prepare docs.path rewrite");
        }
        while (true) {
            const int rc = sqlite3_step(select);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW)
                throw std::runtime_error("cannot read docs.path");
            const sqlite3_int64 id = sqlite3_column_int64(select, 0);
            const auto* raw = sqlite3_column_text(select, 1);
            const fs::path source = fromUtf8(
                raw ? reinterpret_cast<const char*>(raw) : "");
            const fs::path target = rebasePath(source, mappings);
            const std::string targetUtf8 = utf8(target);
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
            sqlite3_bind_text(
                update, 1, targetUtf8.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, id);
            if (sqlite3_step(update) != SQLITE_DONE)
                throw std::runtime_error("cannot update docs.path");
        }
        sqlite3_finalize(select);
        select = nullptr;
        sqlite3_finalize(update);
        update = nullptr;
        sqliteExec(handle, "COMMIT");
        sqliteExec(handle, "PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (...) {
        if (select)
            sqlite3_finalize(select);
        if (update)
            sqlite3_finalize(update);
        try { sqliteExec(handle, "ROLLBACK"); } catch (...) {}
        throw;
    }
    std::string verifyError;
    if (!verifySQLiteDatabase(database, verifyError))
        throw std::runtime_error(
            "rewritten inverted index failed integrity check: " + verifyError);
}

void rewriteSettingsForArchive(
    const fs::path& settingsPath,
    const std::vector<PathMapping>& mappings,
    const fs::path& prmMonthlyDirectory,
    const fs::path& prdMonthlyDirectory)
{
    rewriteSettings(
        settingsPath, mappings,
        prmMonthlyDirectory, prdMonthlyDirectory, "archive");
}

void rewriteSettingsForActive(
    const fs::path& settingsPath,
    const std::vector<PathMapping>& archiveToOriginalMappings,
    const fs::path& prmMonthlyDirectory,
    const fs::path& prdMonthlyDirectory)
{
    rewriteSettings(
        settingsPath, archiveToOriginalMappings,
        prmMonthlyDirectory, prdMonthlyDirectory, "active");
}

ServiceArchiveResult executeServiceArchive(
    const ServiceArchivePlan& plan,
    const ProgressCallback& progress)
{
    ServiceArchiveResult result;
    result.archiveDirectory = plan.finalDirectory;
    result.manifestPath = plan.finalDirectory / kManifestName;
    bool serviceStopped = false;
    bool imageSwitched = false;
    bool published = false;
    try {
        ScManager manager(SC_MANAGER_CONNECT);
        ServiceHandle service = openServiceForArchive(
            manager.get(), plan.service.serviceName);
        if (queryImagePath(service.get()) != plan.service.imagePath)
            throw std::runtime_error("service ImagePath changed after planning");

        if (progress)
            progress(L"Остановка службы " + plan.service.serviceName);
        stopService(service.get());
        serviceStopped = true;

        fs::create_directories(plan.options.archiveRoot);
        const fs::path staging = plan.options.archiveRoot /
            (L"." + plan.finalDirectory.filename().wstring() +
             L".staging-" + std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(staging))
            throw std::runtime_error("service archive staging already exists");
        fs::create_directories(staging);

        std::vector<CopiedFile> copied;
        const std::vector<fs::path> excludedMonthlyDirectories{
            plan.originalPrmMonthlyDirectory,
            plan.originalPrdMonthlyDirectory};
        for (const auto& mapping : plan.mappings) {
            if (progress)
                progress(L"Копирование каталога: " + mapping.source.wstring());
            copyTree(
                mapping.source,
                staging / mapping.target.lexically_relative(plan.finalDirectory),
                plan.year,
                copied,
                excludedMonthlyDirectories);
        }

        for (const auto& database : plan.monthlyDatabases) {
            const fs::path target = staging / database.relativeTarget;
            if (progress)
                progress(L"SQLite backup: " + database.source.wstring());
            fs::create_directories(target.parent_path());
            const SQLiteBackupResult backup = backupSQLiteDatabase(
                database.source, target);
            if (!backup.ok)
                throw std::runtime_error("monthly SQLite backup failed: " + backup.message);
            rewriteAutoPadDirectTo(target, plan.mappings);
            std::string verifyError;
            if (!verifySQLiteDatabase(target, verifyError))
                throw std::runtime_error("rewritten monthly SQLite failed: " + verifyError);
        }

        const fs::path stagedData = staging /
            plan.archivedDataDirectory.lexically_relative(plan.finalDirectory);
        const fs::path stagedSettings = stagedData / L"Settings.json";
        const fs::path stagedIndex = stagedData / L"inverted_index.sqlite";
        if (!fs::is_regular_file(stagedSettings) ||
            !fs::is_regular_file(stagedIndex))
        {
            throw std::runtime_error(
                "staged server data has no Settings.json/inverted_index.sqlite");
        }
        if (progress)
            progress(L"Перепривязка путей поискового индекса");
        rewriteDocumentCatalogPaths(stagedIndex, plan.mappings);
        const fs::path archivedPrmMonthlyDirectory =
            plan.originalPrmMonthlyDirectory.empty()
            ? fs::path{}
            : plan.finalDirectory / L"autopad" / L"PRM" / L"monthly";
        const fs::path archivedPrdMonthlyDirectory =
            plan.originalPrdMonthlyDirectory.empty()
            ? fs::path{}
            : plan.finalDirectory / L"autopad" / L"PRD" / L"monthly";
        if (!archivedPrmMonthlyDirectory.empty()) {
            fs::create_directories(
                staging / archivedPrmMonthlyDirectory.lexically_relative(
                    plan.finalDirectory));
        }
        if (!archivedPrdMonthlyDirectory.empty()) {
            fs::create_directories(
                staging / archivedPrdMonthlyDirectory.lexically_relative(
                    plan.finalDirectory));
        }
        rewriteSettingsForArchive(
            stagedSettings,
            plan.mappings,
            archivedPrmMonthlyDirectory,
            archivedPrdMonthlyDirectory);

        json manifest;
        manifest["format_version"] = 1;
        manifest["operation"] = "service-archive";
        manifest["phase"] = "published";
        manifest["year"] = plan.year;
        manifest["service_name"] = utf8(plan.service.serviceName);
        manifest["original_image_path"] = utf8(plan.service.imagePath);
        manifest["archived_image_path"] = utf8(plan.archivedImagePath);
        manifest["original_data_directory"] = utf8(plan.service.dataDirectory);
        manifest["archived_data_directory"] = utf8(plan.archivedDataDirectory);
        manifest["original_executable"] = utf8(plan.service.executable);
        manifest["archived_executable"] = utf8(plan.archivedExecutable);
        manifest["warnings"] = plan.warnings;
        manifest["original_prm_monthly_directory"] =
            utf8(plan.originalPrmMonthlyDirectory);
        manifest["original_prd_monthly_directory"] =
            utf8(plan.originalPrdMonthlyDirectory);
        manifest["mappings"] = mappingJson(plan.mappings);
        manifest["files"] = json::array();
        for (const auto& file : copied) {
            const fs::path finalTarget = plan.finalDirectory /
                file.stagedTarget.lexically_relative(staging);
            const FileHashResult target = sha256File(file.stagedTarget);
            if (!target.ok)
                throw std::runtime_error(target.message);
            manifest["files"].push_back({
                {"source", utf8(file.source)},
                {"target", utf8(finalTarget)},
                {"source_size", file.original.size},
                {"source_sha256", file.original.sha256},
                {"target_size", target.size},
                {"target_sha256", target.sha256},
                {"transformed", target.sha256 != file.original.sha256}});
        }
        manifest["monthly_databases"] = json::array();
        for (const auto& database : plan.monthlyDatabases) {
            const SQLiteSourceFingerprint fingerprint =
                inspectSQLiteSource(database.source);
            if (!fingerprint.ok)
                throw std::runtime_error(fingerprint.message);
            const FileHashResult source = sha256File(database.source);
            if (!source.ok)
                throw std::runtime_error(source.message);
            const fs::path stagedTarget = staging / database.relativeTarget;
            const FileHashResult target = sha256File(stagedTarget);
            if (!target.ok)
                throw std::runtime_error(target.message);
            manifest["monthly_databases"].push_back({
                {"kind", database.kind == MonthlyDatabase::Kind::Prm ? "PRM" : "PRD"},
                {"month", database.month},
                {"source", utf8(database.source)},
                {"target", utf8(plan.finalDirectory / database.relativeTarget)},
                {"source_fingerprint", fingerprint.value},
                {"source_fingerprint_cacheable", fingerprint.cacheable},
                {"source_journal_mode", fingerprint.journal_mode},
                {"source_size", source.size},
                {"source_sha256", source.sha256},
                {"target_size", target.size},
                {"target_sha256", target.sha256}});
        }
        saveJsonAtomically(staging / kManifestName, manifest);

        std::error_code publishError;
        fs::rename(staging, plan.finalDirectory, publishError);
        if (publishError)
            throw std::runtime_error(
                "cannot publish service archive: " + publishError.message());
        published = true;

        if (progress)
            progress(L"Переключение SCM на архивную копию");
        setImagePath(service.get(), plan.archivedImagePath);
        imageSwitched = true;
        manifest["phase"] = "scm-switched";
        saveJsonAtomically(result.manifestPath, manifest);
        startService(service.get());
        serviceStopped = false;
        manifest["phase"] = "archive-running";
        saveJsonAtomically(result.manifestPath, manifest);

        result.ok = true;
        result.message =
            "service is running from verified archive; original sources are preserved";
        return result;
    } catch (const std::exception& error) {
        std::string rollbackError;
        try {
            if (imageSwitched || serviceStopped) {
                ScManager manager(SC_MANAGER_CONNECT);
                ServiceHandle service = openServiceForArchive(
                    manager.get(), plan.service.serviceName);
                stopService(service.get());
                if (imageSwitched)
                    setImagePath(service.get(), plan.service.imagePath);
                if (plan.service.currentState == SERVICE_RUNNING)
                    startService(service.get());
            }
        } catch (const std::exception& rollback) {
            rollbackError = rollback.what();
        }
        if (published) {
            try {
                json manifest = readJson(result.manifestPath);
                manifest["phase"] = rollbackError.empty()
                    ? "failed-rolled-back" : "failed-rollback-required";
                manifest["failure"] = error.what();
                if (!rollbackError.empty())
                    manifest["rollback_failure"] = rollbackError;
                saveJsonAtomically(result.manifestPath, manifest);
            } catch (...) {}
        }
        result.message = error.what();
        if (!rollbackError.empty())
            result.message += "; rollback failed: " + rollbackError;
        return result;
    }
}

ServiceArchiveResult cleanupServiceArchiveSources(
    const fs::path& archiveDirectory,
    bool deleteMonthlyDatabases,
    const ProgressCallback& progress)
{
    ServiceArchiveResult result;
    result.archiveDirectory = absoluteNormalized(archiveDirectory);
    result.manifestPath = result.archiveDirectory / kManifestName;
    try {
        if (!result.archiveDirectory.is_absolute() ||
            isDriveRoot(result.archiveDirectory))
        {
            throw std::runtime_error("unsafe service archive directory");
        }
        json manifest = readJson(result.manifestPath);
        if (manifest.value("operation", "") != "service-archive" ||
            manifest.value("phase", "") != "archive-running")
        {
            throw std::runtime_error(
                "service archive is not ready for source cleanup");
        }

        const std::wstring serviceName = encoding::utf8_to_wstring(
            manifest.at("service_name").get<std::string>());
        const std::wstring archivedImage = encoding::utf8_to_wstring(
            manifest.at("archived_image_path").get<std::string>());
        ScManager manager(SC_MANAGER_CONNECT);
        ServiceHandle service = openServiceForArchive(manager.get(), serviceName);
        if (queryImagePath(service.get()) != archivedImage)
            throw std::runtime_error("service no longer points to this archive");

        const auto mappings = mappingsFromJson(manifest.at("mappings"), false);
        if (mappings.empty())
            throw std::runtime_error("service archive manifest has no mappings");
        for (const auto& mapping : mappings) {
            if (!mapping.source.is_absolute() || isDriveRoot(mapping.source) ||
                !isPathEqualOrBelow(mapping.target, result.archiveDirectory))
            {
                throw std::runtime_error("unsafe path mapping in service manifest");
            }
        }
        ensureNoOtherServiceUsesSources(serviceName, mappings);

        const fs::path archivedData = fromUtf8(
            manifest.at("archived_data_directory").get<std::string>());
        if (!isPathEqualOrBelow(archivedData, result.archiveDirectory))
            throw std::runtime_error("unsafe archived data directory in manifest");
        const fs::path archivedIndex = archivedData / L"inverted_index.sqlite";
        std::string verifyError;
        if (!verifySQLiteDatabase(archivedIndex, verifyError)) {
            throw std::runtime_error(
                "archived inverted index failed integrity check: " + verifyError);
        }
        const json archivedSettings = readJson(archivedData / L"Settings.json");
        if (!archivedSettings.contains("config") ||
            archivedSettings.at("config").value("server_mode", "") != "archive")
        {
            throw std::runtime_error("archived Settings.json is not frozen");
        }

        std::vector<fs::path> removableFiles;
        for (const auto& item : manifest.at("files")) {
            const fs::path source = fromUtf8(
                item.at("source").get<std::string>());
            const fs::path target = fromUtf8(
                item.at("target").get<std::string>());
            const fs::path expected = rebasePath(source, mappings);
            if (!samePath(expected, target) ||
                !isPathEqualOrBelow(target, result.archiveDirectory))
            {
                throw std::runtime_error(
                    "file path does not match service archive mapping: " +
                    utf8(source));
            }
            if (!fs::is_regular_file(target)) {
                throw std::runtime_error(
                    "archived target file is missing: " + utf8(target));
            }
            if (!fs::exists(source))
                continue;
            const FileHashResult sourceHash = sha256File(source);
            if (!sourceHash.ok ||
                sourceHash.size != item.at("source_size").get<std::uint64_t>() ||
                sourceHash.sha256 != item.at("source_sha256").get<std::string>())
            {
                throw std::runtime_error(
                    "source changed after archive publication; cleanup refused: " +
                    utf8(source));
            }

            // Program and indexed-content files are immutable in archive mode.
            // Data files may legitimately change while the archived service runs.
            if (!isPathEqualOrBelow(target, archivedData)) {
                const FileHashResult targetHash = sha256File(target);
                if (!targetHash.ok ||
                    targetHash.size != item.at("target_size").get<std::uint64_t>() ||
                    targetHash.sha256 != item.at("target_sha256").get<std::string>())
                {
                    throw std::runtime_error(
                        "archived immutable file changed; cleanup refused: " +
                        utf8(target));
                }
            }
            removableFiles.push_back(source);
        }

        std::vector<fs::path> removableMonthlyDatabases;
        if (deleteMonthlyDatabases) {
            for (const auto& item : manifest.at("monthly_databases")) {
                const std::string kind = item.at("kind").get<std::string>();
                const int month = item.at("month").get<int>();
                if (kind == "PRD" && month == 12)
                    continue;
                const fs::path source = fromUtf8(
                    item.at("source").get<std::string>());
                const fs::path target = fromUtf8(
                    item.at("target").get<std::string>());
                if (!source.is_absolute() || isDriveRoot(source) ||
                    !isPathEqualOrBelow(target, result.archiveDirectory) ||
                    !fs::is_regular_file(target))
                {
                    throw std::runtime_error(
                        "unsafe or missing monthly database in manifest");
                }
                if (!verifySQLiteDatabase(target, verifyError)) {
                    throw std::runtime_error(
                        "archived monthly database failed integrity check: " +
                        verifyError);
                }
                const FileHashResult targetHash = sha256File(target);
                if (!targetHash.ok ||
                    targetHash.size != item.at("target_size").get<std::uint64_t>() ||
                    targetHash.sha256 != item.at("target_sha256").get<std::string>())
                {
                    throw std::runtime_error(
                        "archived monthly database changed: " + utf8(target));
                }
                if (!fs::exists(source))
                    continue;
                const FileHashResult sourceHash = sha256File(source);
                const SQLiteSourceFingerprint fingerprint =
                    inspectSQLiteSource(source);
                if (!sourceHash.ok || !fingerprint.ok ||
                    sourceHash.size != item.at("source_size").get<std::uint64_t>() ||
                    sourceHash.sha256 != item.at("source_sha256").get<std::string>() ||
                    fingerprint.value !=
                        item.at("source_fingerprint").get<std::string>())
                {
                    throw std::runtime_error(
                        "source monthly database changed; cleanup refused: " +
                        utf8(source));
                }
                removableMonthlyDatabases.push_back(source);
            }
        }

        // Every target and every unchanged source is validated before the first
        // deletion. Missing files from an earlier interrupted cleanup are allowed.
        std::vector<fs::path> deletedFiles;
        for (const auto& source : removableFiles) {
            if (progress)
                progress(L"Удаление проверенного исходного файла: " + source.wstring());
            std::error_code error;
            if (!fs::remove(source, error) || error)
                throw std::runtime_error("cannot delete source file: " + utf8(source));
            deletedFiles.push_back(source);
        }
        for (const auto& source : removableMonthlyDatabases) {
            if (progress)
                progress(L"Удаление проверенной месячной базы: " + source.wstring());
            std::error_code error;
            if (!fs::remove(source, error) || error) {
                throw std::runtime_error(
                    "cannot delete source monthly database: " + utf8(source));
            }
            deletedFiles.push_back(source);
        }

        std::set<fs::path> candidateDirectories;
        for (const auto& file : deletedFiles) {
            fs::path current = file.parent_path();
            for (const auto& mapping : mappings) {
                if (!isPathEqualOrBelow(current, mapping.source))
                    continue;
                while (isPathEqualOrBelow(current, mapping.source)) {
                    if (!samePath(current, fs::path(L"D:\\TLG")))
                        candidateDirectories.insert(current);
                    if (samePath(current, mapping.source))
                        break;
                    current = current.parent_path();
                }
                break;
            }
        }
        std::vector<fs::path> orderedDirectories(
            candidateDirectories.begin(), candidateDirectories.end());
        std::sort(
            orderedDirectories.begin(), orderedDirectories.end(),
            [](const fs::path& left, const fs::path& right) {
                return left.wstring().size() > right.wstring().size();
            });
        for (const auto& directory : orderedDirectories) {
            std::error_code error;
            fs::remove(directory, error); // Empty directories only.
        }

        manifest["phase"] = "archive-running-source-cleaned";
        manifest["source_cleanup_deleted_files"] = deletedFiles.size();
        manifest["source_cleanup_deleted_monthly_databases"] =
            removableMonthlyDatabases.size();
        saveJsonAtomically(result.manifestPath, manifest);
        result.ok = true;
        result.message =
            "manifest-proven service source cleanup completed; PRD December preserved";
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
}

ServiceArchiveResult restoreServiceArchive(
    const fs::path& archiveDirectory,
    const ProgressCallback& progress)
{
    ServiceArchiveResult result;
    result.archiveDirectory = absoluteNormalized(archiveDirectory);
    result.manifestPath = result.archiveDirectory / kManifestName;
    try {
        json manifest = readJson(result.manifestPath);
        const std::string phase = manifest.value("phase", "");
        if (manifest.value("operation", "") != "service-archive" ||
            (phase != "archive-running" &&
             phase != "archive-running-source-cleaned"))
        {
            throw std::runtime_error(
                "service archive is not in a restorable running phase");
        }
        const std::wstring serviceName = encoding::utf8_to_wstring(
            manifest.at("service_name").get<std::string>());
        const std::wstring originalImage = encoding::utf8_to_wstring(
            manifest.at("original_image_path").get<std::string>());
        const std::wstring archivedImage = encoding::utf8_to_wstring(
            manifest.at("archived_image_path").get<std::string>());
        const ServiceInvocation original = parseServiceInvocation(originalImage);
        ScManager manager(SC_MANAGER_CONNECT);
        ServiceHandle service = openServiceForArchive(manager.get(), serviceName);
        if (queryImagePath(service.get()) != archivedImage)
            throw std::runtime_error("service no longer points to this archive");
        if (progress)
            progress(L"Остановка архивной службы " + serviceName);
        stopService(service.get());
        bool imageSwitched = false;
        try {
            if (phase == "archive-running") {
                if (!fs::is_regular_file(original.executable) ||
                    !fs::is_regular_file(
                        original.dataDirectory / L"Settings.json") ||
                    !fs::is_regular_file(
                        original.dataDirectory / L"inverted_index.sqlite"))
                {
                    throw std::runtime_error(
                        "preserved original server copy is incomplete");
                }
            } else {
                const int year = manifest.at("year").get<int>();
                const auto mappings = mappingsFromJson(
                    manifest.at("mappings"), false);
                const auto reverseMappings = mappingsFromJson(
                    manifest.at("mappings"), true);
                if (mappings.empty())
                    throw std::runtime_error("service archive has no path mappings");

                struct RestoreTree {
                    PathMapping mapping;
                    fs::path staging;
                };
                std::vector<RestoreTree> restoreTrees;
                std::vector<PathMapping> originalToStaging;
                std::size_t ordinal = 0;
                for (const auto& mapping : mappings) {
                    if (!mapping.source.is_absolute() ||
                        isDriveRoot(mapping.source) ||
                        !isPathEqualOrBelow(
                            mapping.target, result.archiveDirectory) ||
                        !fs::is_directory(mapping.target))
                    {
                        throw std::runtime_error(
                            "unsafe or missing restore mapping");
                    }
                    fs::path leaf = mapping.source.filename();
                    if (leaf.empty())
                        leaf = L"root";
                    const fs::path staging = mapping.source.parent_path() /
                        (L"." + leaf.wstring() + L".restore-" +
                         std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(++ordinal));
                    if (fs::exists(staging)) {
                        throw std::runtime_error(
                            "restore staging already exists: " + utf8(staging));
                    }
                    if (progress)
                        progress(L"Подготовка возврата: " + mapping.source.wstring());
                    std::vector<CopiedFile> copied;
                    copyTree(mapping.target, staging, year, copied);
                    restoreTrees.push_back({mapping, staging});
                    originalToStaging.push_back({mapping.source, staging});
                }

                const fs::path stagedData = rebasePath(
                    original.dataDirectory, originalToStaging);
                const fs::path stagedSettings = stagedData / L"Settings.json";
                const fs::path stagedIndex = stagedData / L"inverted_index.sqlite";
                const fs::path archivedData = fromUtf8(
                    manifest.at("archived_data_directory").get<std::string>());
                if (!fs::is_regular_file(stagedSettings) ||
                    !fs::is_regular_file(stagedIndex) ||
                    !isPathEqualOrBelow(archivedData, result.archiveDirectory))
                {
                    throw std::runtime_error(
                        "archive copy has no restorable Settings/index");
                }

                // Normalize the live archived WAL database into a standalone
                // snapshot before paths are changed back to the active roots.
                const fs::path indexSnapshot = stagedIndex.wstring() + L".snapshot";
                const SQLiteBackupResult indexBackup = backupSQLiteDatabase(
                    archivedData / L"inverted_index.sqlite", indexSnapshot);
                if (!indexBackup.ok)
                    throw std::runtime_error(
                        "cannot snapshot archived index: " + indexBackup.message);
                std::error_code fileError;
                fs::remove(stagedIndex, fileError);
                if (fileError)
                    throw std::runtime_error("cannot replace staged index");
                fs::remove(stagedIndex.wstring() + L"-wal", fileError);
                fileError.clear();
                fs::remove(stagedIndex.wstring() + L"-shm", fileError);
                fileError.clear();
                fs::rename(indexSnapshot, stagedIndex, fileError);
                if (fileError)
                    throw std::runtime_error(
                        "cannot publish staged index snapshot: " +
                        fileError.message());

                if (progress)
                    progress(L"Возврат путей поискового индекса");
                rewriteDocumentCatalogPaths(stagedIndex, reverseMappings);
                rewriteSettingsForActive(
                    stagedSettings,
                    reverseMappings,
                    fromUtf8(manifest.value(
                        "original_prm_monthly_directory", std::string())),
                    fromUtf8(manifest.value(
                        "original_prd_monthly_directory", std::string())));

                struct MonthlyRestore {
                    fs::path source;
                    fs::path staging;
                };
                std::vector<MonthlyRestore> monthlyRestores;
                for (const auto& item : manifest.at("monthly_databases")) {
                    const fs::path source = fromUtf8(
                        item.at("source").get<std::string>());
                    const fs::path archived = fromUtf8(
                        item.at("target").get<std::string>());
                    if (fs::exists(source))
                        continue; // Includes the protected PRD December database.
                    if (!source.is_absolute() || isDriveRoot(source) ||
                        !isPathEqualOrBelow(archived, result.archiveDirectory) ||
                        !fs::is_regular_file(archived))
                    {
                        throw std::runtime_error(
                            "unsafe or missing monthly database restore path");
                    }
                    fs::create_directories(source.parent_path());
                    const fs::path staging = source.parent_path() /
                        (L"." + source.filename().wstring() + L".restore-" +
                         std::to_wstring(GetCurrentProcessId()));
                    if (fs::exists(staging))
                        throw std::runtime_error(
                            "monthly restore staging already exists");
                    const SQLiteBackupResult backup = backupSQLiteDatabase(
                        archived, staging);
                    if (!backup.ok)
                        throw std::runtime_error(
                            "monthly restore backup failed: " + backup.message);
                    rewriteAutoPadDirectTo(staging, reverseMappings);
                    std::string verifyError;
                    if (!verifySQLiteDatabase(staging, verifyError)) {
                        throw std::runtime_error(
                            "restored monthly database failed integrity check: " +
                            verifyError);
                    }
                    monthlyRestores.push_back({source, staging});
                }

                for (const auto& tree : restoreTrees) {
                    if (isTlgRoot(tree.mapping.source)) {
                        publishMergedTree(tree.staging, tree.mapping.source);
                        continue;
                    }
                    if (fs::exists(tree.mapping.source)) {
                        if (directoryTreesEqual(
                                tree.mapping.source, tree.staging))
                        {
                            continue; // Safe retry after an interrupted publish.
                        }
                        removeEmptyDirectoryTree(tree.mapping.source);
                    }
                    std::error_code publishError;
                    fs::rename(tree.staging, tree.mapping.source, publishError);
                    if (publishError) {
                        throw std::runtime_error(
                            "cannot publish restored directory: " +
                            publishError.message());
                    }
                }
                for (const auto& database : monthlyRestores) {
                    if (fs::exists(database.source)) {
                        const FileHashResult existing = sha256File(database.source);
                        const FileHashResult staged = sha256File(database.staging);
                        if (existing.ok && staged.ok &&
                            existing.size == staged.size &&
                            existing.sha256 == staged.sha256)
                        {
                            continue;
                        }
                        throw std::runtime_error(
                            "monthly restore destination appeared during publish");
                    }
                    std::error_code publishError;
                    fs::rename(database.staging, database.source, publishError);
                    if (publishError) {
                        throw std::runtime_error(
                            "cannot publish restored monthly database: " +
                            publishError.message());
                    }
                }

                std::string verifyError;
                if (!verifySQLiteDatabase(
                        original.dataDirectory / L"inverted_index.sqlite",
                        verifyError))
                {
                    throw std::runtime_error(
                        "restored inverted index failed integrity check: " +
                        verifyError);
                }
            }

            setImagePath(service.get(), originalImage);
            imageSwitched = true;
            startService(service.get());
            if (queryImagePath(service.get()) != originalImage)
                throw std::runtime_error(
                    "restored service ImagePath verification failed");
            if (queryStatus(service.get()).dwCurrentState != SERVICE_RUNNING)
                throw std::runtime_error(
                    "restored service did not reach SERVICE_RUNNING");
            if (!fs::is_regular_file(original.executable) ||
                !fs::is_directory(original.dataDirectory) ||
                !fs::is_regular_file(original.dataDirectory / L"Settings.json") ||
                !fs::is_regular_file(
                    original.dataDirectory / L"inverted_index.sqlite"))
            {
                throw std::runtime_error(
                    "restored executable or data directory is incomplete");
            }
            ensureServicesDoNotUseArchive(
                result.archiveDirectory,
                enumerateSearchEngineServices());
        } catch (...) {
            if (imageSwitched)
                setImagePath(service.get(), archivedImage);
            try { startService(service.get()); } catch (...) {}
            throw;
        }
        manifest["phase"] = "restored-running";
        saveJsonAtomically(result.manifestPath, manifest);
        result.ok = true;
        result.message = phase == "archive-running"
            ? "service is running from the preserved original active copy"
            : "service files were restored and the active copy is running";
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
}

ServiceArchiveResult validateRestoredServiceArchiveDeletion(
    const fs::path& archiveDirectory,
    const std::vector<InstalledService>& installedServices)
{
    ServiceArchiveResult result;
    result.archiveDirectory = absoluteNormalized(archiveDirectory);
    result.manifestPath = result.archiveDirectory / kManifestName;
    try {
        if (!result.archiveDirectory.is_absolute() ||
            isDriveRoot(result.archiveDirectory) ||
            isDriveRoot(result.archiveDirectory.parent_path()) ||
            !fs::is_directory(result.archiveDirectory))
        {
            throw std::runtime_error(
                "archive deletion requires an exact safe service directory");
        }
        if (!fs::is_regular_file(result.manifestPath))
            throw std::runtime_error("archive-operation.json is missing");

        const json manifest = readJson(result.manifestPath);
        if (manifest.value("operation", "") != "service-archive" ||
            manifest.value("phase", "") != "restored-running")
        {
            throw std::runtime_error(
                "service archive is not in restored-running phase");
        }

        const std::wstring serviceName = encoding::utf8_to_wstring(
            manifest.at("service_name").get<std::string>());
        const int year = manifest.at("year").get<int>();
        const fs::path expectedDirectory = result.archiveDirectory.parent_path() /
            (safeServiceLeaf(serviceName) + L"-" + std::to_wstring(year));
        if (!samePath(expectedDirectory, result.archiveDirectory))
            throw std::runtime_error(
                "archive directory name does not match service manifest");

        const std::wstring originalImage = encoding::utf8_to_wstring(
            manifest.at("original_image_path").get<std::string>());
        const std::wstring archivedImage = encoding::utf8_to_wstring(
            manifest.at("archived_image_path").get<std::string>());
        const ServiceInvocation original = parseServiceInvocation(originalImage);
        const ServiceInvocation archived = parseServiceInvocation(archivedImage);
        const fs::path originalExecutable = fromUtf8(
            manifest.at("original_executable").get<std::string>());
        const fs::path originalData = fromUtf8(
            manifest.at("original_data_directory").get<std::string>());
        const fs::path archivedExecutable = fromUtf8(
            manifest.at("archived_executable").get<std::string>());
        const fs::path archivedData = fromUtf8(
            manifest.at("archived_data_directory").get<std::string>());
        if (!samePath(original.executable, originalExecutable) ||
            !samePath(original.dataDirectory, originalData) ||
            !samePath(archived.executable, archivedExecutable) ||
            !samePath(archived.dataDirectory, archivedData) ||
            !isStrictlyBelow(archivedExecutable, result.archiveDirectory) ||
            !isStrictlyBelow(archivedData, result.archiveDirectory) ||
            !fs::is_regular_file(archivedExecutable) ||
            !fs::is_directory(archivedData))
        {
            throw std::runtime_error(
                "service paths do not match the archive manifest");
        }

        if (!fs::is_regular_file(originalExecutable) ||
            !fs::is_directory(originalData) ||
            !fs::is_regular_file(originalData / L"Settings.json") ||
            !fs::is_regular_file(originalData / L"inverted_index.sqlite"))
        {
            throw std::runtime_error(
                "restored executable or data directory is incomplete");
        }

        const InstalledService* selected = nullptr;
        for (const auto& service : installedServices) {
            if (service.serviceName == serviceName) {
                if (selected)
                    throw std::runtime_error("duplicate selected service snapshot");
                selected = &service;
            }
        }
        if (!selected)
            throw std::runtime_error("restored SearchEngine service is missing");
        if (selected->imagePath != originalImage ||
            !samePath(selected->executable, originalExecutable) ||
            !samePath(selected->dataDirectory, originalData))
        {
            throw std::runtime_error(
                "restored service ImagePath does not match the manifest");
        }
        if (selected->currentState != SERVICE_RUNNING)
            throw std::runtime_error("restored service is not SERVICE_RUNNING");

        const auto requireArchivePath = [&](const fs::path& path, const char* label) {
            if (!path.is_absolute() ||
                !isStrictlyBelow(path, result.archiveDirectory))
            {
                throw std::runtime_error(
                    std::string(label) + " escapes archiveDirectory");
            }
        };
        for (const auto& mapping : manifest.at("mappings")) {
            requireArchivePath(
                fromUtf8(mapping.at("target").get<std::string>()),
                "mapping target");
        }
        for (const auto& item : manifest.at("files")) {
            requireArchivePath(
                fromUtf8(item.at("target").get<std::string>()),
                "file target");
        }
        for (const auto& item : manifest.at("monthly_databases")) {
            requireArchivePath(
                fromUtf8(item.at("target").get<std::string>()),
                "monthly database target");
        }

        ensureServicesDoNotUseArchive(
            result.archiveDirectory,
            installedServices);
        ensureArchiveTreeHasNoReparsePoints(result.archiveDirectory);

        result.ok = true;
        result.message =
            "restored service is running from the original location; "
            "archive deletion checks passed";
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
}

ServiceArchiveResult deleteRestoredServiceArchive(
    const fs::path& archiveDirectory,
    const std::vector<InstalledService>& installedServices,
    const ProgressCallback& progress)
{
    ServiceArchiveResult result = validateRestoredServiceArchiveDeletion(
        archiveDirectory,
        installedServices);
    if (!result.ok)
        return result;
    if (progress)
        progress(L"Удаление проверенной архивной копии: " +
                 result.archiveDirectory.wstring());
    std::error_code error;
    fs::remove_all(result.archiveDirectory, error);
    if (error || fs::exists(result.archiveDirectory)) {
        result.ok = false;
        result.message =
            "restored service remains active at the original location; "
            "archive deletion failed or was incomplete: " + error.message();
        return result;
    }
    result.ok = true;
    result.message = "verified service archive copy was deleted";
    return result;
}

ServiceArchiveResult deleteRestoredServiceArchive(
    const fs::path& archiveDirectory,
    const ProgressCallback& progress)
{
    return deleteRestoredServiceArchive(
        archiveDirectory,
        enumerateSearchEngineServices(),
        progress);
}

} // namespace searchengine_archive
