#include "Backup/Restore/RestoreInterfaces.h"

#include "Backup/FileHash.h"
#include "Backup/Restore/MirrorHistoryStore.h"
#include "MyUtils/Encoding.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace backup_restore_detail {

namespace fs = std::filesystem;

struct NullProgress : public IRestoreProgress {
    void onPhase(const std::string&) override {}
    void onFile(size_t, size_t, const std::string&) override {}
    void onWarning(const std::string&) override {}
    bool isCancelled() const override { return false; }
};

std::atomic<unsigned long long> staging_sequence{0};

fs::path uniqueStagingPath(const fs::path& parent)
{
    for (;;) {
        const auto sequence = staging_sequence.fetch_add(1);
        const fs::path candidate =
            parent /
            (".backuprestore.partial_" + std::to_string(sequence));
        std::error_code error;
        if (!fs::exists(candidate, error)) {
            return candidate;
        }
    }
}

bool pathAllowed(
    const std::string& path,
    const std::vector<std::string>& filter)
{
    if (filter.empty()) {
        return true;
    }
    return std::find(filter.begin(), filter.end(), path) != filter.end();
}

RestorePlanSource toPlanSource(RestoreResolveStatus status)
{
    switch (status) {
    case RestoreResolveStatus::InCurrent:
        return RestorePlanSource::CurrentData;
    case RestoreResolveStatus::InObjects:
        return RestorePlanSource::ObjectStore;
    case RestoreResolveStatus::Missing:
        return RestorePlanSource::Missing;
    }
    return RestorePlanSource::Missing;
}

const char* statusText(RestoreResolveStatus status)
{
    switch (status) {
    case RestoreResolveStatus::InCurrent:
        return "current";
    case RestoreResolveStatus::InObjects:
        return "objects";
    case RestoreResolveStatus::Missing:
        return "missing";
    }
    return "missing";
}

bool copyFileBinary(
    const fs::path& source,
    const fs::path& destination,
    std::string& error)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        error =
            "cannot create directory \"" +
            pathToUtf8Path(destination.parent_path()) + "\": " +
            encoding::system_error_to_utf8(ec.message());
        return false;
    }

    std::ifstream input(source, std::ios::binary);
    if (!input.is_open()) {
        error =
            "cannot open source \"" + pathToUtf8Path(source) + '"';
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error =
            "cannot create \"" + pathToUtf8Path(destination) + '"';
        return false;
    }
    output << input.rdbuf();
    if (!output.good() || input.bad()) {
        error =
            "I/O error copying \"" + pathToUtf8Path(source) + '"';
        return false;
    }
    return true;
}

bool replaceDirectory(
    const fs::path& source,
    const fs::path& destination,
    std::string& error)
{
#ifdef _WIN32
    if (MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_WRITE_THROUGH
        ) != 0)
    {
        return true;
    }
    // Fallback: destination missing — try rename via filesystem
#endif
    std::error_code ec;
    fs::rename(source, destination, ec);
    if (!ec) {
        return true;
    }
    error =
        "cannot publish restored directory: " +
        encoding::system_error_to_utf8(ec.message());
    return false;
}

RestorePointInfo pointFromManifest(
    const fs::path& target_root,
    const std::string& target_id,
    const fs::path& manifest_path,
    const MirrorManifest& manifest,
    bool is_current)
{
    RestorePointInfo point;
    point.target_id = target_id;
    point.target_root = target_root;
    point.manifest_path = manifest_path;
    point.is_current = is_current;
    point.complete = manifest.complete;
    point.source_path = manifest.source;
    point.error_count = manifest.errors.size();
    point.file_count = manifest.files.size();
    for (const auto& file : manifest.files) {
        point.total_size += file.size;
    }

    if (is_current) {
        point.tier = "current";
        point.label = "current";
        point.unix_seconds = manifest.updated_unix_seconds;
        if (point.unix_seconds <= 0 && !manifest.updated_at.empty()) {
            point.label = manifest.updated_at;
        }
    } else {
        point.tier = manifest.point_tier.empty()
            ? manifest_path.parent_path().parent_path().filename().string()
            : manifest.point_tier;
        point.label = manifest.point_created_at.empty()
            ? manifest_path.parent_path().filename().string()
            : manifest.point_created_at;
        point.unix_seconds = manifest.point_created_unix_seconds > 0
            ? manifest.point_created_unix_seconds
            : manifest.updated_unix_seconds;
    }

    formatLocalDateTime(
        point.unix_seconds,
        point.date_local,
        point.time_local
    );
    return point;
}

class MirrorHistoryRestoreService final
    : public IBackupStoreScanner,
      public IRestorePointCatalog,
      public IRestoreFileCatalog,
      public IRestorePlanner,
      public IRestoreVerifier,
      public IRestoreExecutor,
      public std::enable_shared_from_this<MirrorHistoryRestoreService> {
public:
    std::vector<RestoreTargetInfo> scanRoot(
        const fs::path& backup_root,
        std::string& error) override
    {
        error.clear();
        std::vector<RestoreTargetInfo> result;
        std::error_code ec;
        if (!fs::is_directory(backup_root, ec) || ec) {
            error =
                "backup root is not a directory: " +
                pathToUtf8Path(backup_root);
            return result;
        }

        for (fs::directory_iterator it(backup_root, ec), end;
             !ec && it != end;
             it.increment(ec))
        {
            if (!it->is_directory(ec)) {
                continue;
            }
            const fs::path target_root = it->path();
            if (!looksLikeMirrorTargetRoot(target_root)) {
                continue;
            }

            RestoreTargetInfo info;
            info.id = pathToUtf8Path(target_root.filename());
            info.display_name = displayNameFromTargetId(info.id);
            info.root_path = target_root;
            const fs::path current_manifest =
                target_root / "current" / "manifest.json";
            if (fs::exists(current_manifest, ec) && !ec) {
                const MirrorManifest manifest =
                    readMirrorManifest(current_manifest);
                if (manifest.ok) {
                    info.has_current = true;
                    info.file_count_current = manifest.files.size();
                    for (const auto& file : manifest.files) {
                        info.total_size_current += file.size;
                    }
                }
            }
            result.push_back(std::move(info));
        }

        std::sort(
            result.begin(),
            result.end(),
            [](const RestoreTargetInfo& lhs, const RestoreTargetInfo& rhs) {
                return lhs.display_name < rhs.display_name;
            }
        );
        return result;
    }

    std::vector<RestorePointInfo> listPoints(
        const RestoreTargetInfo& target,
        std::string& error) override
    {
        error.clear();
        std::vector<RestorePointInfo> points;

        const fs::path current_manifest =
            target.root_path / "current" / "manifest.json";
        std::error_code ec;
        if (fs::exists(current_manifest, ec) && !ec) {
            const MirrorManifest manifest =
                readMirrorManifest(current_manifest);
            if (!manifest.ok) {
                error = manifest.error_message;
                return {};
            }
            points.push_back(pointFromManifest(
                target.root_path,
                target.id,
                current_manifest,
                manifest,
                true
            ));
        }

        const fs::path restore_points = target.root_path / "restore_points";
        if (fs::is_directory(restore_points, ec) && !ec) {
            for (fs::directory_iterator tier_it(restore_points, ec), tier_end;
                 !ec && tier_it != tier_end;
                 tier_it.increment(ec))
            {
                if (!tier_it->is_directory(ec)) {
                    continue;
                }
                for (fs::directory_iterator point_it(tier_it->path(), ec),
                         point_end;
                     !ec && point_it != point_end;
                     point_it.increment(ec))
                {
                    if (!point_it->is_directory(ec)) {
                        continue;
                    }
                    const fs::path manifest_path =
                        point_it->path() / "manifest.json";
                    if (!fs::exists(manifest_path, ec) || ec) {
                        continue;
                    }
                    const MirrorManifest manifest =
                        readMirrorManifest(manifest_path);
                    if (!manifest.ok) {
                        continue;
                    }
                    points.push_back(pointFromManifest(
                        target.root_path,
                        target.id,
                        manifest_path,
                        manifest,
                        false
                    ));
                }
            }
        }

        std::sort(
            points.begin(),
            points.end(),
            [](const RestorePointInfo& lhs, const RestorePointInfo& rhs) {
                if (lhs.unix_seconds != rhs.unix_seconds) {
                    return lhs.unix_seconds > rhs.unix_seconds;
                }
                if (lhs.is_current != rhs.is_current) {
                    return lhs.is_current;
                }
                return lhs.label > rhs.label;
            }
        );
        return points;
    }

    RestorePointInfo currentPoint(
        const RestoreTargetInfo& target,
        std::string& error) override
    {
        error.clear();
        const fs::path current_manifest =
            target.root_path / "current" / "manifest.json";
        const MirrorManifest manifest =
            readMirrorManifest(current_manifest);
        if (!manifest.ok) {
            error = manifest.error_message;
            return {};
        }
        return pointFromManifest(
            target.root_path,
            target.id,
            current_manifest,
            manifest,
            true
        );
    }

    bool findTarget(
        const fs::path& backup_root,
        const std::string& id_or_path,
        RestoreTargetInfo& target,
        std::string& error) override
    {
        error.clear();
        const auto targets = scanRoot(backup_root, error);
        if (!error.empty()) {
            return false;
        }

        std::error_code ec;
        fs::path as_path = pathFromUtf8Path(id_or_path);
        if (!fs::path(id_or_path).is_absolute()) {
            // keep as_path from utf8
        }
        if (fs::exists(as_path, ec) && looksLikeMirrorTargetRoot(as_path)) {
            for (const auto& candidate : targets) {
                if (fs::equivalent(candidate.root_path, as_path, ec)) {
                    target = candidate;
                    return true;
                }
            }
            target.id = pathToUtf8Path(as_path.filename());
            target.display_name = displayNameFromTargetId(target.id);
            target.root_path = as_path;
            target.has_current =
                fs::exists(as_path / "current" / "manifest.json", ec);
            return true;
        }

        for (const auto& candidate : targets) {
            if (candidate.id == id_or_path ||
                candidate.display_name == id_or_path)
            {
                target = candidate;
                return true;
            }
        }
        error = "target not found: " + id_or_path;
        return false;
    }

    bool loadPointFromManifest(
        const fs::path& manifest_path,
        RestorePointInfo& point,
        std::string& error) override
    {
        error.clear();
        const MirrorManifest manifest = readMirrorManifest(manifest_path);
        if (!manifest.ok) {
            error = manifest.error_message;
            return false;
        }

        fs::path target_root = manifest_path.parent_path();
        bool is_current = false;
        if (target_root.filename() == "current") {
            target_root = target_root.parent_path();
            is_current = true;
        } else {
            // <target>/restore_points/<tier>/<label>/manifest.json
            target_root = manifest_path.parent_path()  // label
                              .parent_path()          // tier
                              .parent_path()          // restore_points
                              .parent_path();         // target
        }

        const std::string target_id =
            pathToUtf8Path(target_root.filename());
        point = pointFromManifest(
            target_root,
            target_id,
            manifest_path,
            manifest,
            is_current
        );
        return true;
    }

    std::vector<RestoreFileInfo> listFiles(
        const RestorePointInfo& point,
        std::string& error) override
    {
        error.clear();
        const MirrorManifest manifest =
            readMirrorManifest(point.manifest_path);
        if (!manifest.ok) {
            error = manifest.error_message;
            return {};
        }

        std::vector<RestoreFileInfo> files;
        files.reserve(manifest.files.size());
        for (const auto& entry : manifest.files) {
            RestoreFileInfo info;
            info.relative_path = entry.path;
            info.size = entry.size;
            info.sha256 = entry.sha256;
            info.captured_at = entry.captured_at;
            info.method = entry.method;
            resolveManifestFile(
                point.target_root,
                entry,
                info.resolve_status,
                info.resolved_path
            );
            files.push_back(std::move(info));
        }
        std::sort(
            files.begin(),
            files.end(),
            [](const RestoreFileInfo& lhs, const RestoreFileInfo& rhs) {
                return lhs.relative_path < rhs.relative_path;
            }
        );
        return files;
    }

    std::vector<RestorePlanEntry> plan(
        const RestorePointInfo& point,
        const std::vector<std::string>& path_filter,
        std::string& error) override
    {
        error.clear();
        const MirrorManifest manifest =
            readMirrorManifest(point.manifest_path);
        if (!manifest.ok) {
            error = manifest.error_message;
            return {};
        }

        std::vector<RestorePlanEntry> entries;
        for (const auto& file : manifest.files) {
            if (!pathAllowed(file.path, path_filter)) {
                continue;
            }
            RestorePlanEntry entry;
            entry.path = file.path;
            entry.size = file.size;
            entry.sha256 = file.sha256;
            RestoreResolveStatus status = RestoreResolveStatus::Missing;
            resolveManifestFile(
                point.target_root,
                file,
                status,
                entry.source_path
            );
            entry.source = toPlanSource(status);
            entry.status = statusText(status);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    bool verify(
        const RestorePointInfo& point,
        const std::vector<std::string>& path_filter,
        IRestoreProgress* progress,
        std::string& error) override
    {
        error.clear();
        NullProgress null_progress;
        IRestoreProgress& sink =
            progress != nullptr ? *progress : null_progress;

        if (!point.complete) {
            sink.onWarning(
                "manifest marked complete=false; some files may be stale"
            );
        }

        const auto entries = plan(point, path_filter, error);
        if (!error.empty()) {
            return false;
        }

        sink.onPhase("verify");
        size_t index = 0;
        for (const auto& entry : entries) {
            if (sink.isCancelled()) {
                error = "cancelled";
                return false;
            }
            ++index;
            sink.onFile(index, entries.size(), entry.path);
            if (entry.source == RestorePlanSource::Missing) {
                error = "missing data for \"" + entry.path + '"';
                return false;
            }
            const FileHashResult hash = sha256File(entry.source_path);
            if (!hash.ok) {
                error = hash.message;
                return false;
            }
            if (hash.size != entry.size || hash.sha256 != entry.sha256) {
                error =
                    "hash mismatch for \"" + entry.path +
                    "\" at \"" + pathToUtf8Path(entry.source_path) + '"';
                return false;
            }
        }
        return true;
    }

    bool restore(
        const RestoreRequest& request,
        IRestoreProgress* progress,
        std::string& error) override
    {
        error.clear();
        NullProgress null_progress;
        IRestoreProgress& sink =
            progress != nullptr ? *progress : null_progress;

        if (request.destination.empty()) {
            error = "destination is required";
            return false;
        }

        std::error_code ec;
        if (fs::exists(request.destination, ec) && !ec) {
            if (!request.overwrite) {
                error =
                    "destination exists (pass overwrite to replace): " +
                    pathToUtf8Path(request.destination);
                return false;
            }
        }

        if (!request.point.complete) {
            sink.onWarning(
                "manifest marked complete=false; restoring best available"
            );
        }

        const auto entries =
            plan(request.point, request.path_filter, error);
        if (!error.empty()) {
            return false;
        }
        if (entries.empty()) {
            error = "nothing to restore";
            return false;
        }
        for (const auto& entry : entries) {
            if (entry.source == RestorePlanSource::Missing) {
                error = "missing data for \"" + entry.path + '"';
                return false;
            }
        }

        const fs::path parent =
            request.destination.has_parent_path()
                ? request.destination.parent_path()
                : fs::path(".");
        fs::create_directories(parent, ec);
        if (ec) {
            error =
                "cannot create destination parent: " +
                encoding::system_error_to_utf8(ec.message());
            return false;
        }

        const fs::path staging = uniqueStagingPath(parent);
        fs::create_directories(staging, ec);
        if (ec) {
            error =
                "cannot create staging directory: " +
                encoding::system_error_to_utf8(ec.message());
            return false;
        }

        const auto cleanup = [&]() {
            std::error_code remove_error;
            fs::remove_all(staging, remove_error);
        };

        sink.onPhase("copy");
        size_t index = 0;
        for (const auto& entry : entries) {
            if (sink.isCancelled()) {
                cleanup();
                error = "cancelled";
                return false;
            }
            ++index;
            sink.onFile(index, entries.size(), entry.path);
            const fs::path out =
                staging / pathFromUtf8Path(entry.path);
            if (!copyFileBinary(entry.source_path, out, error)) {
                cleanup();
                return false;
            }
        }

        // Recreate empty directories from current/historical current manifest
        {
            const MirrorManifest manifest =
                readMirrorManifest(request.point.manifest_path);
            if (manifest.ok) {
                for (const auto& directory : manifest.directories) {
                    if (!request.path_filter.empty()) {
                        continue;
                    }
                    fs::create_directories(
                        staging / pathFromUtf8Path(directory),
                        ec
                    );
                }
            }
        }

        sink.onPhase("verify-staging");
        index = 0;
        for (const auto& entry : entries) {
            if (sink.isCancelled()) {
                cleanup();
                error = "cancelled";
                return false;
            }
            ++index;
            sink.onFile(index, entries.size(), entry.path);
            const fs::path out =
                staging / pathFromUtf8Path(entry.path);
            const FileHashResult hash = sha256File(out);
            if (!hash.ok) {
                cleanup();
                error = hash.message;
                return false;
            }
            if (hash.size != entry.size || hash.sha256 != entry.sha256) {
                cleanup();
                error =
                    "staging hash mismatch for \"" + entry.path + '"';
                return false;
            }
        }

        sink.onPhase("publish");
        if (fs::exists(request.destination, ec) && !ec) {
            fs::path retired = request.destination;
            retired += ".before_restore";
            size_t suffix = 1;
            while (fs::exists(retired, ec) && !ec) {
                retired = request.destination;
                retired +=
                    ".before_restore_" + std::to_string(suffix++);
            }
            fs::rename(request.destination, retired, ec);
            if (ec) {
                cleanup();
                error =
                    "cannot retire existing destination: " +
                    encoding::system_error_to_utf8(ec.message());
                return false;
            }
        }

        if (!replaceDirectory(staging, request.destination, error)) {
            cleanup();
            return false;
        }
        return true;
    }
};

RestoreServices createMirrorHistoryRestoreServicesImpl()
{
    auto service = std::make_shared<MirrorHistoryRestoreService>();
    RestoreServices result;
    result.scanner = service;
    result.points = service;
    result.files = service;
    result.planner = service;
    result.verifier = service;
    result.executor = service;
    return result;
}

} // namespace backup_restore_detail

RestoreServices createMirrorHistoryRestoreServices()
{
    return backup_restore_detail::createMirrorHistoryRestoreServicesImpl();
}
