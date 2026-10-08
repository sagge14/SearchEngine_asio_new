#include "Backup/Restore/MirrorHistoryStore.h"

#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

namespace fs = std::filesystem;
namespace nh = nlohmann;

std::string relativeKey(const fs::path& path)
{
    return encoding::wstring_to_utf8(
        path.lexically_normal().generic_wstring()
    );
}

} // namespace

bool isSafeRelativeUtf8Path(const std::string& configured)
{
    if (configured.empty()) {
        return false;
    }
    const fs::path path = pathFromUtf8Path(configured);
    if (path.empty() ||
        path.is_absolute() ||
        path.has_root_name() ||
        path.has_root_directory())
    {
        return false;
    }
    for (const fs::path& component : path) {
        if (component.empty() ||
            component == fs::path(".") ||
            component == fs::path(".."))
        {
            return false;
        }
    }
    return relativeKey(path) == configured;
}

fs::path pathFromUtf8Path(const std::string& path)
{
    return fs::path(encoding::utf8_to_wstring(path));
}

std::string pathToUtf8Path(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

fs::path mirrorObjectPath(
    const fs::path& target_root,
    const std::string& relative,
    const std::string& sha256)
{
    return target_root / "objects" / "by-path" /
        pathFromUtf8Path(relative) / sha256;
}

fs::path mirrorCurrentDataPath(
    const fs::path& target_root,
    const std::string& relative)
{
    return target_root / "current" / "data" / pathFromUtf8Path(relative);
}

void formatLocalDateTime(
    std::int64_t unix_seconds,
    std::string& date_local,
    std::string& time_local)
{
    date_local.clear();
    time_local.clear();
    if (unix_seconds <= 0) {
        return;
    }
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm local_tm{};
#ifdef _WIN32
    if (localtime_s(&local_tm, &value) != 0) {
        return;
    }
#else
    if (localtime_r(&value, &local_tm) == nullptr) {
        return;
    }
#endif
    std::ostringstream date_stream;
    date_stream
        << std::setfill('0')
        << std::setw(4) << (local_tm.tm_year + 1900) << '-'
        << std::setw(2) << (local_tm.tm_mon + 1) << '-'
        << std::setw(2) << local_tm.tm_mday;
    date_local = date_stream.str();

    std::ostringstream time_stream;
    time_stream
        << std::setfill('0')
        << std::setw(2) << local_tm.tm_hour << ':'
        << std::setw(2) << local_tm.tm_min << ':'
        << std::setw(2) << local_tm.tm_sec;
    time_local = time_stream.str();
}

std::string displayNameFromTargetId(const std::string& id)
{
    const size_t underscore = id.rfind('_');
    if (underscore == std::string::npos || underscore == 0) {
        return id;
    }
    const std::string suffix = id.substr(underscore + 1);
    if (suffix.size() == 16 &&
        std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        }))
    {
        return id.substr(0, underscore);
    }
    return id;
}

bool looksLikeMirrorTargetRoot(const fs::path& path)
{
    std::error_code error;
    if (!fs::is_directory(path, error) || error) {
        return false;
    }
    if (fs::exists(path / "current" / "manifest.json", error) && !error) {
        return true;
    }
    const fs::path restore_points = path / "restore_points";
    if (!fs::is_directory(restore_points, error) || error) {
        return false;
    }
    for (fs::directory_iterator it(restore_points, error), end;
         !error && it != end;
         it.increment(error))
    {
        if (!fs::is_directory(it->path(), error) || error) {
            continue;
        }
        for (fs::directory_iterator point_it(it->path(), error), point_end;
             !error && point_it != point_end;
             point_it.increment(error))
        {
            if (fs::is_directory(point_it->path(), error) && !error &&
                fs::exists(point_it->path() / "manifest.json", error) &&
                !error)
            {
                return true;
            }
        }
    }
    return false;
}

MirrorManifest readMirrorManifest(const fs::path& manifest_path)
{
    MirrorManifest result;
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input.is_open()) {
        result.error_message =
            "cannot open manifest \"" + pathToUtf8Path(manifest_path) + '"';
        return result;
    }

    try {
        nh::json root;
        input >> root;
        if (root.at("format").get<int>() != 1 ||
            root.at("strategy").get<std::string>() != "mirror_history" ||
            !root.at("files").is_array() ||
            !root.at("directories").is_array())
        {
            result.error_message = "unsupported mirror_history manifest format";
            return result;
        }

        result.complete = root.value("complete", false);
        result.source = root.value("source", std::string{});
        result.updated_unix_seconds =
            root.value("updated_unix_seconds", std::int64_t{0});
        result.updated_at = root.value("updated_at", std::string{});
        result.point_created_unix_seconds =
            root.value("point_created_unix_seconds", std::int64_t{0});
        result.point_created_at =
            root.value("point_created_at", std::string{});
        result.point_tier = root.value("point_tier", std::string{});
        result.directories =
            root.at("directories").get<std::vector<std::string>>();
        result.errors =
            root.value("errors", std::vector<std::string>{});

        for (const auto& directory : result.directories) {
            if (!isSafeRelativeUtf8Path(directory)) {
                result.error_message =
                    "unsafe directory path in manifest";
                return result;
            }
        }

        for (const auto& item : root.at("files")) {
            MirrorManifestFile file;
            file.path = item.at("path").get<std::string>();
            file.size = item.at("size").get<std::uint64_t>();
            file.sha256 = item.at("sha256").get<std::string>();
            file.captured_at = item.value("captured_at", std::string{});
            file.method = item.value("method", std::string{});
            if (!isSafeRelativeUtf8Path(file.path) ||
                file.sha256.size() != 64 ||
                !std::all_of(
                    file.sha256.begin(),
                    file.sha256.end(),
                    [](unsigned char ch) {
                        return std::isxdigit(ch) != 0;
                    }))
            {
                result.error_message = "invalid file entry in manifest";
                return result;
            }
            result.files.push_back(std::move(file));
        }

        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message =
            std::string("invalid manifest: ") + exception.what();
        return result;
    }
}

bool resolveManifestFile(
    const fs::path& target_root,
    const MirrorManifestFile& file,
    RestoreResolveStatus& status,
    fs::path& resolved_path)
{
    status = RestoreResolveStatus::Missing;
    resolved_path.clear();

    std::error_code error;
    const fs::path object =
        mirrorObjectPath(target_root, file.path, file.sha256);
    if (fs::is_regular_file(object, error) && !error) {
        status = RestoreResolveStatus::InObjects;
        resolved_path = object;
        return true;
    }

    const fs::path current =
        mirrorCurrentDataPath(target_root, file.path);
    if (fs::is_regular_file(current, error) && !error) {
        status = RestoreResolveStatus::InCurrent;
        resolved_path = current;
        return true;
    }

    return false;
}
