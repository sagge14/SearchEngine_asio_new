#include "ArchiveCore.h"

#include "Backup/FileHash.h"
#include "Backup/SQLiteBackup.h"
#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace searchengine_archive {
namespace {

using json = nlohmann::json;

constexpr wchar_t kManifestName[] = L"archive-operation.json";
constexpr wchar_t kHashListName[] = L"files.sha256";

std::string utf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

fs::path fromUtf8(const std::string& value)
{
    return fs::path(encoding::utf8_to_wstring(value));
}

std::wstring normalizedWindowsPath(const fs::path& value)
{
    std::error_code error;
    fs::path absolute = fs::absolute(value, error);
    if (error)
        absolute = value;
    std::wstring result = absolute.lexically_normal().wstring();
    while (result.size() > 3 &&
           (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    std::replace(result.begin(), result.end(), L'/', L'\\');
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

bool pathComponentBoundary(const std::wstring& value, std::size_t offset)
{
    return offset == value.size() ||
        (offset > 0 && value[offset - 1] == L'\\') ||
        value[offset] == L'\\';
}

fs::path absoluteNormalizedPreservingCase(const fs::path& value)
{
    std::error_code error;
    fs::path result = fs::absolute(value, error);
    if (error)
        result = value;
    return result.lexically_normal();
}

std::wstring lowerPathComponent(const fs::path& value)
{
    std::wstring result = value.wstring();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

fs::path withoutTrailingSeparator(const fs::path& path);

fs::path relativePathCaseInsensitive(
    const fs::path& value,
    const fs::path& root)
{
    const fs::path normalizedValue =
        withoutTrailingSeparator(absoluteNormalizedPreservingCase(value));
    const fs::path normalizedRoot =
        withoutTrailingSeparator(absoluteNormalizedPreservingCase(root));
    auto valuePart = normalizedValue.begin();
    const auto valueEnd = normalizedValue.end();
    for (auto rootPart = normalizedRoot.begin();
         rootPart != normalizedRoot.end();
         ++rootPart, ++valuePart)
    {
        if (valuePart == valueEnd ||
            lowerPathComponent(*valuePart) != lowerPathComponent(*rootPart))
        {
            throw std::runtime_error(
                "path is outside the selected mapping root: " + utf8(value));
        }
    }

    fs::path relative;
    for (; valuePart != valueEnd; ++valuePart) {
        if (!valuePart->empty() && *valuePart != L".")
            relative /= *valuePart;
    }
    return relative;
}

bool isDriveRoot(const fs::path& path)
{
    const fs::path normalized = path.lexically_normal();
    return !normalized.root_path().empty() && normalized == normalized.root_path();
}

fs::path withoutTrailingSeparator(const fs::path& path)
{
    fs::path result = path.lexically_normal();
    while (result != result.root_path() && !result.has_filename())
        result = result.parent_path();
    return result;
}

bool isTlgRoot(const fs::path& path)
{
    return normalizedWindowsPath(path) == L"d:\\tlg";
}

bool isBelowTlg(const fs::path& path)
{
    return isPathEqualOrBelow(path, fs::path(L"D:\\TLG"));
}

fs::path safeLeaf(const fs::path& source)
{
    fs::path leaf = source.filename();
    if (leaf.empty())
        leaf = source.root_name().wstring() + L"_root";
    std::wstring value = leaf.wstring();
    for (wchar_t& ch : value) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return fs::path(value.empty() ? L"root" : value);
}

class SqliteHandle final {
public:
    SqliteHandle(const fs::path& path, int flags)
    {
        const std::string pathUtf8 = utf8(path);
        const int rc = sqlite3_open_v2(pathUtf8.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string detail = db_ ? sqlite3_errmsg(db_) : "no handle";
            if (db_)
                sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error(
                "cannot open SQLite database '" + pathUtf8 + "': " + detail);
        }
        sqlite3_busy_timeout(db_, 30000);
    }

    ~SqliteHandle()
    {
        if (db_)
            sqlite3_close_v2(db_);
    }

    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;

    sqlite3* get() const noexcept { return db_; }

private:
    sqlite3* db_{};
};

void exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK)
        return;
    const std::string detail = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error("SQLite command failed: " + detail);
}

bool archiveHasRequiredColumns(sqlite3* db)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(archive)", -1, &statement, nullptr) != SQLITE_OK)
        return false;
    bool directTo = false;
    bool fileName = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* raw = sqlite3_column_text(statement, 1);
        std::string name = raw ? reinterpret_cast<const char*>(raw) : "";
        std::transform(
            name.begin(), name.end(), name.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        directTo = directTo || name == "directto";
        fileName = fileName || name == "filename";
    }
    sqlite3_finalize(statement);
    return directTo && fileName;
}

std::vector<fs::path> directToRoots(const fs::path& database)
{
    SqliteHandle handle(database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    if (!archiveHasRequiredColumns(handle.get())) {
        throw std::runtime_error(
            "AutoPad database has no archive.DirectTo/FileName columns: " +
            utf8(database));
    }

    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT DISTINCT DirectTo FROM archive "
        "WHERE DirectTo IS NOT NULL AND TRIM(DirectTo) <> ''";
    if (sqlite3_prepare_v2(handle.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot read DirectTo from " + utf8(database));

    std::vector<fs::path> result;
    while (true) {
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW) {
            const std::string detail = sqlite3_errmsg(handle.get());
            sqlite3_finalize(statement);
            throw std::runtime_error("cannot enumerate DirectTo: " + detail);
        }
        const auto* raw = sqlite3_column_text(statement, 0);
        if (raw) {
            const fs::path path = withoutTrailingSeparator(
                fromUtf8(reinterpret_cast<const char*>(raw)));
            if (!path.empty())
                result.push_back(path);
        }
    }
    sqlite3_finalize(statement);
    return result;
}

void rewriteDirectTo(
    const fs::path& database,
    const std::vector<PathMapping>& mappings)
{
    SqliteHandle handle(database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX);
    if (!archiveHasRequiredColumns(handle.get()))
        throw std::runtime_error("AutoPad schema changed in staged database");

    exec(handle.get(), "BEGIN IMMEDIATE");
    sqlite3_stmt* select = nullptr;
    sqlite3_stmt* update = nullptr;
    try {
        if (sqlite3_prepare_v2(
                handle.get(),
                "SELECT DirectTo, FileName FROM archive "
                "WHERE DirectTo IS NOT NULL AND TRIM(DirectTo) <> ''",
                -1, &select, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(
                "cannot prepare AutoPad path scan: " +
                std::string(sqlite3_errmsg(handle.get())));
        }

        std::map<std::string, std::string> rewrites;
        while (true) {
            const int rc = sqlite3_step(select);
            if (rc == SQLITE_DONE)
                break;
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(
                    "cannot read AutoPad archive row: " +
                    std::string(sqlite3_errmsg(handle.get())));
            }

            const auto* rawDirectory = sqlite3_column_text(select, 0);
            const auto* rawFileName = sqlite3_column_text(select, 1);
            const std::string directory = rawDirectory
                ? reinterpret_cast<const char*>(rawDirectory) : "";
            const std::string fileName = rawFileName
                ? reinterpret_cast<const char*>(rawFileName) : "";

            if (!fileName.empty()) {
                const fs::path name = fromUtf8(fileName);
                if (name.is_absolute() || name.has_root_path()) {
                    throw std::runtime_error(
                        "AutoPad FileName must stay relative: " + fileName);
                }
            }

            fs::path target = rebasePath(fromUtf8(directory), mappings);
            std::wstring targetValue = target.wstring();
            if (!targetValue.empty() && targetValue.back() != L'\\')
                targetValue.push_back(L'\\');
            const std::string targetUtf8 =
                encoding::wstring_to_utf8(targetValue);
            const auto [existing, inserted] = rewrites.emplace(
                directory, targetUtf8);
            if (!inserted && existing->second != targetUtf8) {
                throw std::runtime_error(
                    "one AutoPad DirectTo maps to multiple archive paths: " +
                    directory);
            }
        }

        sqlite3_finalize(select);
        select = nullptr;

        if (sqlite3_prepare_v2(
                handle.get(),
                "UPDATE archive SET DirectTo=? WHERE DirectTo=?",
                -1, &update, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(
                "cannot prepare AutoPad path update: " +
                std::string(sqlite3_errmsg(handle.get())));
        }
        for (const auto& [directory, target] : rewrites) {
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
            sqlite3_bind_text(
                update, 1, target.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(
                update, 2, directory.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(update) != SQLITE_DONE) {
                throw std::runtime_error(
                    "cannot update AutoPad DirectTo: " +
                    std::string(sqlite3_errmsg(handle.get())));
            }
        }
        sqlite3_finalize(update);
        update = nullptr;
        exec(handle.get(), "COMMIT");
    } catch (...) {
        if (select)
            sqlite3_finalize(select);
        if (update)
            sqlite3_finalize(update);
        try { exec(handle.get(), "ROLLBACK"); } catch (...) {}
        throw;
    }
}

std::vector<MonthlyDatabase> discoverDatabases(
    const YearMoveOptions& options,
    std::vector<std::string>* warnings)
{
    std::vector<MonthlyDatabase> result;
    const auto add = [&](MonthlyDatabase::Kind kind, const fs::path& directory) {
        if (directory.empty())
            return;
        const char* kindName = kind == MonthlyDatabase::Kind::Prm ? "PRM" : "PRD";
        if (!fs::is_directory(directory)) {
            if (warnings) {
                warnings->push_back(
                    "Каталог месячных баз " + std::string(kindName) +
                    " не найден; продолжаем без него: " +
                    utf8(directory));
            }
            return;
        }
        std::vector<int> missingMonths;
        std::size_t found = 0;
        for (int month = 1; month <= 13; ++month) {
            std::wostringstream name;
            name << std::setfill(L'0') << std::setw(2) << month
                 << L'-' << options.year << L".db3";
            const fs::path source = directory / name.str();
            if (!fs::is_regular_file(source)) {
                missingMonths.push_back(month);
                continue;
            }
            const fs::path side = kind == MonthlyDatabase::Kind::Prm
                ? fs::path(L"autopad") / L"PRM" / L"monthly"
                : fs::path(L"autopad") / L"PRD" / L"monthly";
            result.push_back({kind, source, side / source.filename(), month});
            ++found;
        }
        if (warnings && !missingMonths.empty()) {
            std::ostringstream warning;
            if (found == 0) {
                warning << "Месячные базы " << kindName << " за "
                        << options.year << " не найдены; продолжаем без них: "
                        << utf8(directory);
            } else {
                warning << "Отсутствуют месячные базы " << kindName << " за "
                        << options.year << ": ";
                for (std::size_t index = 0; index < missingMonths.size(); ++index) {
                    if (index != 0)
                        warning << ", ";
                    warning << std::setfill('0') << std::setw(2)
                            << missingMonths[index];
                }
                warning << "; существующие базы будут перенесены";
            }
            warnings->push_back(warning.str());
        }
    };
    add(MonthlyDatabase::Kind::Prm, options.prmMonthlyDirectory);
    add(MonthlyDatabase::Kind::Prd, options.prdMonthlyDirectory);
    if (result.empty() && warnings) {
        warnings->push_back(
            "Месячные базы выбранного года отсутствуют; "
            "операция продолжится без них");
    }
    return result;
}

struct CopiedFile {
    fs::path source;
    fs::path target;
    std::uint64_t size{};
    std::string sha256;
};

void copyOneFile(
    const fs::path& source,
    const fs::path& target,
    std::vector<CopiedFile>& files)
{
    fs::create_directories(target.parent_path());
    if (fs::exists(target))
        throw std::runtime_error("target collision: " + utf8(target));
    std::error_code error;
    fs::copy_file(source, target, fs::copy_options::none, error);
    if (error)
        throw std::runtime_error("cannot copy '" + utf8(source) + "': " + error.message());
    const auto sourceTime = fs::last_write_time(source, error);
    if (!error)
        fs::last_write_time(target, sourceTime, error);
    const FileHashResult sourceHash = sha256File(source);
    const FileHashResult targetHash = sha256File(target);
    if (!sourceHash.ok || !targetHash.ok ||
        sourceHash.size != targetHash.size || sourceHash.sha256 != targetHash.sha256) {
        throw std::runtime_error("copied file verification failed: " + utf8(source));
    }
    files.push_back({source, target, sourceHash.size, sourceHash.sha256});
}

void copyTree(
    const fs::path& source,
    const fs::path& target,
    int archivedYear,
    std::vector<CopiedFile>& files)
{
    if (!fs::is_directory(source))
        throw std::runtime_error("source directory does not exist: " + utf8(source));
    fs::create_directories(target);
    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
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
        if (it->is_directory()) {
            fs::create_directories(destination);
        } else if (it->is_regular_file()) {
            copyOneFile(it->path(), destination, files);
        } else if (it->is_symlink()) {
            throw std::runtime_error("symbolic links/reparse points are not moved: " + utf8(it->path()));
        }
    }
}

json planJson(const YearMovePlan& plan)
{
    json value;
    value["format_version"] = 1;
    value["operation"] = "year-only";
    value["phase"] = "published";
    value["year"] = plan.options.year;
    value["final_directory"] = utf8(plan.finalDirectory);
    value["prm_monthly_directory"] = utf8(plan.options.prmMonthlyDirectory);
    value["prd_monthly_directory"] = utf8(plan.options.prdMonthlyDirectory);
    value["warnings"] = plan.warnings;
    value["mappings"] = json::array();
    for (const auto& mapping : plan.mappings) {
        value["mappings"].push_back({
            {"source", utf8(mapping.source)},
            {"target", utf8(mapping.target)}});
    }
    value["monthly_databases"] = json::array();
    for (const auto& database : plan.databases) {
        value["monthly_databases"].push_back({
            {"kind", database.kind == MonthlyDatabase::Kind::Prm ? "PRM" : "PRD"},
            {"month", database.month},
            {"source", utf8(database.source)},
            {"target", utf8(plan.finalDirectory / database.relativeTarget)}});
    }
    return value;
}

void saveJsonAtomically(const fs::path& path, const json& value)
{
    const fs::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot write manifest: " + utf8(temporary));
        output << value.dump(2) << '\n';
        if (!output)
            throw std::runtime_error("cannot finish manifest: " + utf8(temporary));
    }
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            "cannot publish manifest: Win32 error " +
            std::to_string(GetLastError()));
    }
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error)
        throw std::runtime_error("cannot publish manifest: " + error.message());
#endif
}

} // namespace

bool isPathEqualOrBelow(const fs::path& candidate, const fs::path& root)
{
    const std::wstring value = normalizedWindowsPath(candidate);
    const std::wstring prefix = normalizedWindowsPath(root);
    return value.size() >= prefix.size() &&
        value.compare(0, prefix.size(), prefix) == 0 &&
        pathComponentBoundary(value, prefix.size());
}

fs::path rebasePath(
    const fs::path& source,
    const std::vector<PathMapping>& mappings)
{
    const PathMapping* best = nullptr;
    std::size_t bestLength = 0;
    for (const auto& mapping : mappings) {
        if (!isPathEqualOrBelow(source, mapping.source))
            continue;
        const std::size_t length = normalizedWindowsPath(mapping.source).size();
        if (length > bestLength) {
            best = &mapping;
            bestLength = length;
        }
    }
    if (!best)
        throw std::runtime_error("path is outside the move plan: " + utf8(source));
    const fs::path relative = relativePathCaseInsensitive(
        source, best->source);
    if (relative.empty())
        return best->target;
    return best->target / relative;
}

bool isTlgArchiveRoot(const fs::path& path)
{
    return isTlgRoot(path);
}

bool shouldSkipTlgTopLevelEntry(
    const fs::path& topLevelName,
    bool isDirectory,
    int archivedYear)
{
    if (!isDirectory)
        return false;
    const std::wstring name = topLevelName.filename().wstring();
    if (_wcsicmp(name.c_str(), L"OUT") == 0)
        return true;
    if (name.size() != 4 || !std::all_of(name.begin(), name.end(), iswdigit))
        return false;
    return std::stoi(name) != archivedYear;
}

bool shouldSkipArchiveTreeEntry(
    const fs::path& sourceRoot,
    const fs::path& entryName,
    int depth,
    bool isDirectory,
    int archivedYear)
{
    return isTlgRoot(sourceRoot) && depth == 0 &&
        shouldSkipTlgTopLevelEntry(entryName, isDirectory, archivedYear);
}

std::vector<fs::path> collapseSourceRoots(std::vector<fs::path> roots)
{
    bool containsTlgChild = false;
    for (const auto& root : roots)
        containsTlgChild = containsTlgChild || isBelowTlg(root);
    if (containsTlgChild) {
        roots.erase(
            std::remove_if(
                roots.begin(), roots.end(),
                [](const fs::path& path) { return isBelowTlg(path); }),
            roots.end());
        roots.push_back(fs::path(L"D:\\TLG"));
    }

    std::sort(
        roots.begin(), roots.end(),
        [](const fs::path& left, const fs::path& right) {
            return normalizedWindowsPath(left) < normalizedWindowsPath(right);
        });
    roots.erase(
        std::unique(
            roots.begin(), roots.end(),
            [](const fs::path& left, const fs::path& right) {
                return normalizedWindowsPath(left) == normalizedWindowsPath(right);
            }),
        roots.end());

    std::vector<fs::path> collapsed;
    for (const auto& root : roots) {
        if (root.empty() || !root.is_absolute() || isDriveRoot(root))
            throw std::runtime_error("unsafe or non-absolute DirectTo: " + utf8(root));
        const bool nested = std::any_of(
            collapsed.begin(), collapsed.end(),
            [&](const fs::path& parent) { return isPathEqualOrBelow(root, parent); });
        if (!nested)
            collapsed.push_back(withoutTrailingSeparator(root));
    }
    return collapsed;
}

std::vector<MonthlyDatabase> inspectMonthlyDatabases(
    const YearMoveOptions& options,
    std::vector<std::string>* warnings)
{
    return discoverDatabases(options, warnings);
}

std::vector<fs::path> inspectAutoPadDirectToRoots(
    const fs::path& database)
{
    return directToRoots(database);
}

void rewriteAutoPadDirectTo(
    const fs::path& database,
    const std::vector<PathMapping>& mappings)
{
    rewriteDirectTo(database, mappings);
}

YearMovePlan planYearMove(const YearMoveOptions& options)
{
    if (options.year < 1900 || options.year > 9999)
        throw std::runtime_error("year must be inside 1900..9999");
    if (options.archiveRoot.empty() || !options.archiveRoot.is_absolute() ||
        isDriveRoot(options.archiveRoot)) {
        throw std::runtime_error("archive root must be a safe absolute directory");
    }

    YearMovePlan plan;
    plan.options = options;
    plan.options.archiveRoot = fs::absolute(options.archiveRoot).lexically_normal();
    plan.finalDirectory = plan.options.archiveRoot / std::to_wstring(options.year);
    if (fs::exists(plan.finalDirectory))
        throw std::runtime_error("archive year directory already exists: " + utf8(plan.finalDirectory));

    plan.databases = discoverDatabases(plan.options, &plan.warnings);
    std::vector<fs::path> roots;
    for (const auto& database : plan.databases) {
        std::string error;
        if (!verifySQLiteDatabase(database.source, error))
            throw std::runtime_error("SQLite integrity check failed for " + utf8(database.source) + ": " + error);
        auto databaseRoots = directToRoots(database.source);
        roots.insert(roots.end(), databaseRoots.begin(), databaseRoots.end());
    }
    roots = collapseSourceRoots(std::move(roots));
    if (roots.empty() && !plan.databases.empty())
        throw std::runtime_error("monthly databases contain no DirectTo directories");

    for (const auto& root : roots) {
        if (!fs::is_directory(root))
            throw std::runtime_error("DirectTo directory does not exist: " + utf8(root));
        const fs::path target =
            plan.finalDirectory / L"files" / safeLeaf(root);
        if (isPathEqualOrBelow(target, root) || isPathEqualOrBelow(root, target))
            throw std::runtime_error("source and target trees overlap: " + utf8(root));
        const bool targetCollision = std::any_of(
            plan.mappings.begin(), plan.mappings.end(),
            [&](const PathMapping& mapping) {
                return normalizedWindowsPath(mapping.target) ==
                    normalizedWindowsPath(target);
            });
        if (targetCollision) {
            throw std::runtime_error(
                "archive content roots have the same folder name; "
                "rename one source folder: " + utf8(root.filename()));
        }
        plan.mappings.push_back({root, target});
    }
    return plan;
}

YearMoveResult executeYearMove(
    const YearMovePlan& plan,
    const ProgressCallback& progress)
{
    YearMoveResult result;
    result.finalDirectory = plan.finalDirectory;
    try {
        fs::create_directories(plan.options.archiveRoot);
        const fs::path staging = plan.options.archiveRoot /
            (L"." + std::to_wstring(plan.options.year) + L".staging");
        if (fs::exists(staging))
            throw std::runtime_error("staging directory already exists: " + utf8(staging));
        fs::create_directories(staging);

        std::vector<CopiedFile> copied;
        for (const auto& database : plan.databases) {
            const fs::path destination = staging / database.relativeTarget;
            fs::create_directories(destination.parent_path());
            if (progress)
                progress(L"Копирование SQLite: " + database.source.wstring());
            const SQLiteBackupResult backup = backupSQLiteDatabase(
                database.source, destination);
            if (!backup.ok)
                throw std::runtime_error("SQLite backup failed: " + backup.message);
        }

        for (const auto& mapping : plan.mappings) {
            const fs::path relative = mapping.target.lexically_relative(plan.finalDirectory);
            if (progress)
                progress(L"Копирование каталога: " + mapping.source.wstring());
            copyTree(
                mapping.source,
                staging / relative,
                plan.options.year,
                copied);
        }

        for (const auto& database : plan.databases) {
            if (progress)
                progress(L"Исправление DirectTo: " + database.source.filename().wstring());
            rewriteDirectTo(staging / database.relativeTarget, plan.mappings);
            std::string verifyError;
            if (!verifySQLiteDatabase(staging / database.relativeTarget, verifyError))
                throw std::runtime_error("rewritten SQLite verification failed: " + verifyError);
        }

        json manifest = planJson(plan);
        for (std::size_t index = 0; index < plan.databases.size(); ++index) {
            const auto& database = plan.databases[index];
            const SQLiteSourceFingerprint fingerprint =
                inspectSQLiteSource(database.source);
            if (!fingerprint.ok) {
                throw std::runtime_error(
                    "cannot fingerprint monthly database after backup: " +
                    fingerprint.message);
            }
            const FileHashResult targetHash =
                sha256File(staging / database.relativeTarget);
            if (!targetHash.ok)
                throw std::runtime_error(targetHash.message);
            manifest["monthly_databases"][index]["source_fingerprint"] =
                fingerprint.value;
            manifest["monthly_databases"][index]["source_fingerprint_cacheable"] =
                fingerprint.cacheable;
            manifest["monthly_databases"][index]["source_journal_mode"] =
                fingerprint.journal_mode;
            manifest["monthly_databases"][index]["target_sha256"] =
                targetHash.sha256;
            manifest["monthly_databases"][index]["target_size"] =
                targetHash.size;
        }
        manifest["files"] = json::array();
        std::ofstream hashes(staging / kHashListName, std::ios::binary | std::ios::trunc);
        for (const auto& file : copied) {
            const fs::path finalTarget = plan.finalDirectory /
                file.target.lexically_relative(staging);
            manifest["files"].push_back({
                {"source", utf8(file.source)},
                {"target", utf8(finalTarget)},
                {"size", file.size},
                {"sha256", file.sha256}});
            hashes << file.sha256 << "  " << utf8(finalTarget) << '\n';
            ++result.copiedFiles;
            result.copiedBytes += file.size;
        }
        hashes.close();
        if (!hashes)
            throw std::runtime_error("cannot finish SHA-256 list");
        saveJsonAtomically(staging / kManifestName, manifest);

        std::error_code publishError;
        fs::rename(staging, plan.finalDirectory, publishError);
        if (publishError)
            throw std::runtime_error("cannot publish archive year: " + publishError.message());

        result.ok = true;
        result.manifestPath = plan.finalDirectory / kManifestName;
        result.message = "year archive published; source files were not deleted";
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
}

YearMoveResult cleanupYearMoveFiles(
    const fs::path& finalDirectory,
    bool deleteMonthlyDatabases,
    const ProgressCallback& progress)
{
    YearMoveResult result;
    result.finalDirectory = finalDirectory;
    result.manifestPath = finalDirectory / kManifestName;
    try {
        std::ifstream input(result.manifestPath, std::ios::binary);
        if (!input)
            throw std::runtime_error("archive manifest was not found");
        json manifest;
        input >> manifest;
        input.close();
        const std::string phase = manifest.value("phase", "");
        if (manifest.value("operation", "") != "year-only" ||
            (phase != "published" && phase != "source-files-cleaned")) {
            throw std::runtime_error("archive manifest is not ready for cleanup");
        }

        std::vector<fs::path> removableFiles;
        for (const auto& item : manifest.at("files")) {
            const fs::path source = fromUtf8(item.at("source").get<std::string>());
            const fs::path target = fromUtf8(item.at("target").get<std::string>());
            if (!fs::exists(source))
                continue;
            const FileHashResult sourceHash = sha256File(source);
            const FileHashResult targetHash = sha256File(target);
            if (!sourceHash.ok || !targetHash.ok ||
                sourceHash.sha256 != item.at("sha256").get<std::string>() ||
                targetHash.sha256 != sourceHash.sha256 ||
                sourceHash.size != item.at("size").get<std::uint64_t>()) {
                throw std::runtime_error(
                    "source changed after copy; cleanup stopped before deletion: " +
                    utf8(source));
            }
            removableFiles.push_back(source);
        }

        for (const auto& source : removableFiles) {
            if (progress)
                progress(L"Удаление подтверждённого файла: " + source.wstring());
            std::error_code error;
            if (!fs::remove(source, error) || error)
                throw std::runtime_error("cannot delete source file: " + utf8(source));
        }

        for (const auto& mapping : manifest.at("mappings")) {
            const fs::path source = fromUtf8(mapping.at("source").get<std::string>());
            if (isTlgRoot(source))
                continue;
            std::error_code error;
            fs::remove(source, error); // Empty directories only; never recurse here.
        }

        if (deleteMonthlyDatabases) {
            for (const auto& item : manifest.at("monthly_databases")) {
                const std::string kind = item.at("kind").get<std::string>();
                const int month = item.at("month").get<int>();
                if (kind == "PRD" && month == 12)
                    continue;
                const fs::path source = fromUtf8(item.at("source").get<std::string>());
                const fs::path target = fromUtf8(item.at("target").get<std::string>());
                if (!fs::exists(source))
                    continue;
                const FileHashResult targetHash = sha256File(target);
                if (!targetHash.ok ||
                    targetHash.sha256 != item.at("target_sha256").get<std::string>() ||
                    targetHash.size != item.at("target_size").get<std::uint64_t>()) {
                    throw std::runtime_error("archived monthly database is unavailable: " + utf8(target));
                }
                if (!item.value("source_fingerprint_cacheable", false)) {
                    throw std::runtime_error(
                        "monthly database uses a non-cacheable SQLite journal mode; "
                        "automatic source deletion is forbidden: " + utf8(source));
                }
                const SQLiteSourceFingerprint current = inspectSQLiteSource(source);
                if (!current.ok || !current.cacheable ||
                    current.value != item.at("source_fingerprint").get<std::string>()) {
                    throw std::runtime_error(
                        "source monthly database changed after backup; deletion refused: " +
                        utf8(source));
                }
                std::error_code error;
                if (!fs::remove(source, error) || error)
                    throw std::runtime_error("cannot delete monthly database: " + utf8(source));
            }
        }

        manifest["phase"] = deleteMonthlyDatabases
            ? "source-files-and-monthly-databases-cleaned"
            : "source-files-cleaned";
        saveJsonAtomically(result.manifestPath, manifest);
        result.ok = true;
        result.message = "manifest-proven source cleanup completed";
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
}

} // namespace searchengine_archive
