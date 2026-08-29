#include "BackupConfig.h"

#include "Backup/BackupPathFilter.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace nh = nlohmann;

void addIssue(BackupConfigResult& result,
              std::string location,
              std::string message)
{
    result.issues.push_back(
        BackupConfigIssue{std::move(location), std::move(message)}
    );
}

bool readRequiredString(const nh::json& object,
                        const char* field,
                        const std::string& location,
                        std::string& value,
                        BackupConfigResult& result)
{
    const auto it = object.find(field);
    if (it == object.end()) {
        addIssue(result, location + "." + field, "required field is missing");
        return false;
    }
    if (!it->is_string()) {
        addIssue(result, location + "." + field, "must be a string");
        return false;
    }

    value = it->get<std::string>();
    if (value.empty()) {
        addIssue(result, location + "." + field, "must not be empty");
        return false;
    }
    return true;
}

bool readPositiveSize(const nh::json& object,
                      const char* field,
                      size_t default_value,
                      const std::string& location,
                      size_t& value,
                      BackupConfigResult& result)
{
    const auto it = object.find(field);
    if (it == object.end()) {
        value = default_value;
        return true;
    }

    std::uint64_t unsigned_value = 0;
    if (it->is_number_unsigned()) {
        unsigned_value = it->get<std::uint64_t>();
    } else if (it->is_number_integer()) {
        const std::int64_t signed_value = it->get<std::int64_t>();
        if (signed_value <= 0) {
            addIssue(
                result,
                location + "." + field,
                "must be greater than zero"
            );
            return false;
        }
        unsigned_value = static_cast<std::uint64_t>(signed_value);
    } else {
        addIssue(result, location + "." + field, "must be an integer");
        return false;
    }

    if (unsigned_value == 0 ||
        unsigned_value > std::numeric_limits<size_t>::max())
    {
        addIssue(
            result,
            location + "." + field,
            "is outside the supported range"
        );
        return false;
    }

    value = static_cast<size_t>(unsigned_value);
    return true;
}

bool readOptionalBool(const nh::json& object,
                      const char* field,
                      bool default_value,
                      const std::string& location,
                      bool& value,
                      BackupConfigResult& result)
{
    const auto it = object.find(field);
    if (it == object.end()) {
        value = default_value;
        return true;
    }
    if (!it->is_boolean()) {
        addIssue(result, location + "." + field, "must be true or false");
        return false;
    }
    value = it->get<bool>();
    return true;
}

bool readOptionalMode(const nh::json& object,
                      const std::string& location,
                      BackupMode& value,
                      BackupConfigResult& result)
{
    const auto it = object.find("mode");
    if (it == object.end()) {
        value = BackupMode::Auto;
        return true;
    }
    if (!it->is_string()) {
        addIssue(result, location + ".mode", "must be a string");
        return false;
    }

    std::string mode = it->get<std::string>();
    std::transform(
        mode.begin(),
        mode.end(),
        mode.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    if (mode == "auto") {
        value = BackupMode::Auto;
        return true;
    }
    if (mode == "filesystem") {
        value = BackupMode::Filesystem;
        return true;
    }

    addIssue(
        result,
        location + ".mode",
        "must be \"auto\" or \"filesystem\""
    );
    return false;
}

bool readOptionalStrategy(const nh::json& object,
                          const std::string& location,
                          BackupStrategy& value,
                          BackupConfigResult& result)
{
    const auto it = object.find("strategy");
    if (it == object.end()) {
        value = BackupStrategy::Snapshot;
        return true;
    }
    if (!it->is_string()) {
        addIssue(result, location + ".strategy", "must be a string");
        return false;
    }

    std::string strategy = it->get<std::string>();
    std::transform(
        strategy.begin(),
        strategy.end(),
        strategy.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    if (strategy == "snapshot") {
        value = BackupStrategy::Snapshot;
        return true;
    }
    if (strategy == "mirror_history") {
        value = BackupStrategy::MirrorHistory;
        return true;
    }

    addIssue(
        result,
        location + ".strategy",
        "must be \"snapshot\" or \"mirror_history\""
    );
    return false;
}

std::string trim(std::string value)
{
    const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    value.erase(
        value.begin(),
        std::find_if_not(value.begin(), value.end(), is_space)
    );
    value.erase(
        std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
        value.end()
    );
    return value;
}

bool parseHistoryDuration(const std::string& configured,
                          size_t& seconds,
                          std::string& canonical,
                          std::string& error_message)
{
    std::string value = trim(configured);
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    size_t digit_count = 0;
    while (digit_count < value.size() &&
           std::isdigit(
               static_cast<unsigned char>(value[digit_count])
           ))
    {
        ++digit_count;
    }
    if (digit_count == 0 || digit_count == value.size()) {
        error_message =
            "must use a duration such as 10s, 2min, 3h, 1d, 2w or 1mo";
        return false;
    }

    std::uint64_t count = 0;
    try {
        count = std::stoull(value.substr(0, digit_count));
    } catch (...) {
        error_message = "duration number is outside the supported range";
        return false;
    }
    if (count == 0) {
        error_message = "duration must be greater than zero";
        return false;
    }

    const std::string unit = value.substr(digit_count);
    std::uint64_t multiplier = 0;
    if (unit == "s") {
        multiplier = 1;
    } else if (unit == "min") {
        multiplier = 60;
    } else if (unit == "h") {
        multiplier = 60 * 60;
    } else if (unit == "d") {
        multiplier = 24 * 60 * 60;
    } else if (unit == "w") {
        multiplier = 7 * 24 * 60 * 60;
    } else if (unit == "mo") {
        multiplier = 30ull * 24 * 60 * 60;
    } else {
        error_message =
            "unknown duration unit; use s, min, h, d, w or mo";
        return false;
    }

    if (count >
        std::numeric_limits<std::uint64_t>::max() / multiplier)
    {
        error_message = "duration is outside the supported range";
        return false;
    }
    const std::uint64_t total = count * multiplier;
    if (total > std::numeric_limits<size_t>::max() ||
        total >
            static_cast<std::uint64_t>(
                std::chrono::seconds::max().count()
            ))
    {
        error_message = "duration is too large for the scheduler";
        return false;
    }

    seconds = static_cast<size_t>(total);
    canonical = std::to_string(count) + unit;
    return true;
}

bool addHistoryPeriod(const nh::json& item,
                      const std::string& location,
                      std::set<size_t>& used_periods,
                      std::vector<BackupHistoryTier>& periods,
                      BackupConfigResult& result)
{
    std::string every;
    size_t keep = 1;
    if (item.is_string()) {
        every = item.get<std::string>();
    } else if (item.is_object()) {
        if (!readRequiredString(
                item,
                "every",
                location,
                every,
                result
            ))
        {
            return false;
        }
        if (!readPositiveSize(
                item,
                "keep",
                1,
                location,
                keep,
                result
            ))
        {
            return false;
        }
    } else {
        addIssue(
            result,
            location,
            "must be a duration string or an object with every and keep"
        );
        return false;
    }

    size_t seconds = 0;
    std::string canonical;
    std::string duration_error;
    if (!parseHistoryDuration(
            every,
            seconds,
            canonical,
            duration_error
        ))
    {
        addIssue(result, location, duration_error);
        return false;
    }
    if (!used_periods.insert(seconds).second) {
        addIssue(result, location, "duplicates another history period");
        return false;
    }

    periods.push_back(
        BackupHistoryTier{
            "every_" + canonical,
            seconds,
            keep
        }
    );
    return true;
}

bool readExcludePatterns(const nh::json& object,
                         const std::string& location,
                         bool is_directory,
                         bool is_directory_known,
                         std::vector<std::string>& patterns,
                         BackupConfigResult& result)
{
    const auto it = object.find("exclude");
    if (it == object.end()) {
        patterns.clear();
        return true;
    }
    if (!it->is_array()) {
        addIssue(result, location + ".exclude", "must be an array of strings");
        return false;
    }

    if (is_directory_known && !is_directory && !it->empty()) {
        addIssue(
            result,
            location + ".exclude",
            "is only supported when is_directory is true"
        );
        return false;
    }

    patterns.clear();
    std::set<std::string> seen;
    bool ok = true;
    for (size_t index = 0; index < it->size(); ++index) {
        const std::string item_location =
            location + ".exclude[" + std::to_string(index) + "]";
        const nh::json& item = (*it)[index];
        if (!item.is_string()) {
            addIssue(result, item_location, "must be a string");
            ok = false;
            continue;
        }

        const std::string raw = item.get<std::string>();
        std::string normalized;
        std::string error_message;
        if (!validateExcludePattern(raw, normalized, error_message)) {
            addIssue(result, item_location, error_message);
            ok = false;
            continue;
        }

#ifdef _WIN32
        std::string duplicate_key = normalized;
        std::transform(
            duplicate_key.begin(),
            duplicate_key.end(),
            duplicate_key.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            }
        );
#else
        const std::string& duplicate_key = normalized;
#endif
        if (!seen.insert(duplicate_key).second) {
            addIssue(result, item_location, "duplicate exclude pattern");
            ok = false;
            continue;
        }
        patterns.push_back(std::move(normalized));
    }
    return ok;
}

bool readHistoryPeriods(const nh::json& object,
                        const std::string& location,
                        BackupStrategy strategy,
                        std::vector<BackupHistoryTier>& periods,
                        BackupConfigResult& result)
{
    const auto it = object.find("history_periods");
    if (it == object.end()) {
        if (strategy == BackupStrategy::MirrorHistory) {
            addIssue(
                result,
                location + ".history_periods",
                "is required for mirror_history"
            );
            return false;
        }
        return true;
    }
    if (strategy != BackupStrategy::MirrorHistory) {
        addIssue(
            result,
            location + ".history_periods",
            "is only valid for mirror_history"
        );
        return false;
    }

    std::set<size_t> used_periods;
    if (it->is_string()) {
        const std::string configured = it->get<std::string>();
        size_t start = 0;
        size_t index = 0;
        while (start <= configured.size()) {
            const size_t comma = configured.find(',', start);
            const std::string part = trim(
                configured.substr(
                    start,
                    comma == std::string::npos
                        ? std::string::npos
                        : comma - start
                )
            );
            if (part.empty()) {
                addIssue(
                    result,
                    location + ".history_periods[" +
                        std::to_string(index) + "]",
                    "must not be empty"
                );
                return false;
            }
            addHistoryPeriod(
                nh::json(part),
                location + ".history_periods[" +
                    std::to_string(index) + "]",
                used_periods,
                periods,
                result
            );
            ++index;
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    } else if (it->is_array()) {
        if (it->empty()) {
            addIssue(
                result,
                location + ".history_periods",
                "must not be empty"
            );
            return false;
        }
        for (size_t index = 0; index < it->size(); ++index) {
            addHistoryPeriod(
                (*it)[index],
                location + ".history_periods[" +
                    std::to_string(index) + "]",
                used_periods,
                periods,
                result
            );
        }
    } else {
        addIssue(
            result,
            location + ".history_periods",
            "must be a comma-separated string or an array"
        );
        return false;
    }
    return !periods.empty();
}

std::string normalizedPathForKey(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::absolute(path, error);
    if (error) {
        normalized = path;
    }
    std::string value = normalized.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](char ch) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))
            );
        }
    );
#endif
    return value;
}

} // namespace

BackupConfigResult loadBackupConfig(const std::filesystem::path& path)
{
    BackupConfigResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        addIssue(
            result,
            path.string(),
            "configuration file cannot be opened"
        );
        return result;
    }

    nh::json root;
    try {
        stream >> root;
    } catch (const nh::json::parse_error& error) {
        addIssue(
            result,
            path.string(),
            "JSON parse error at byte " + std::to_string(error.byte)
        );
        return result;
    } catch (const std::exception& error) {
        addIssue(result, path.string(), error.what());
        return result;
    }

    const auto jobs_it = root.find("BackupJobs");
    if (jobs_it == root.end()) {
        addIssue(result, "BackupJobs", "required array is missing");
        return result;
    }
    if (!jobs_it->is_array()) {
        addIssue(result, "BackupJobs", "must be an array");
        return result;
    }

    std::set<std::string> scheduled_targets;
    for (size_t group_index = 0;
         group_index < jobs_it->size();
         ++group_index)
    {
        const nh::json& group_json = (*jobs_it)[group_index];
        const std::string group_location =
            "BackupJobs[" + std::to_string(group_index) + "]";
        const size_t issues_before = result.issues.size();

        if (!group_json.is_object()) {
            addIssue(result, group_location, "must be an object");
            continue;
        }

        BackupGroup group;
        readRequiredString(
            group_json,
            "backup_dir",
            group_location,
            group.backup_dir,
            result
        );
        readPositiveSize(
            group_json,
            "period_sec",
            3600,
            group_location,
            group.period_sec,
            result
        );
        if (group.period_sec >
            static_cast<size_t>(std::chrono::seconds::max().count()))
        {
            addIssue(
                result,
                group_location + ".period_sec",
                "is too large for the scheduler"
            );
        }

        const auto targets_it = group_json.find("targets");
        if (targets_it == group_json.end()) {
            addIssue(
                result,
                group_location + ".targets",
                "required array is missing"
            );
        } else if (!targets_it->is_array()) {
            addIssue(
                result,
                group_location + ".targets",
                "must be an array"
            );
        } else if (targets_it->empty()) {
            addIssue(
                result,
                group_location + ".targets",
                "must not be empty"
            );
        } else {
            for (size_t target_index = 0;
                 target_index < targets_it->size();
                 ++target_index)
            {
                const nh::json& target_json = (*targets_it)[target_index];
                const std::string target_location =
                    group_location + ".targets[" +
                    std::to_string(target_index) + "]";
                if (!target_json.is_object()) {
                    addIssue(result, target_location, "must be an object");
                    continue;
                }

                BackupTarget target;
                std::string source;
                const bool source_ok = readRequiredString(
                    target_json,
                    "src",
                    target_location,
                    source,
                    result
                );
                const bool retention_ok = readPositiveSize(
                    target_json,
                    "max_versions",
                    5,
                    target_location,
                    target.max_versions,
                    result
                );
                const bool type_ok = readOptionalBool(
                    target_json,
                    "is_directory",
                    false,
                    target_location,
                    target.is_directory,
                    result
                );
                const bool mode_ok = readOptionalMode(
                    target_json,
                    target_location,
                    target.mode,
                    result
                );
                const bool strategy_ok = readOptionalStrategy(
                    target_json,
                    target_location,
                    target.strategy,
                    result
                );
                const bool cache_ok = readOptionalBool(
                    target_json,
                    "cache",
                    true,
                    target_location,
                    target.cache,
                    result
                );
                const bool skip_unchanged_ok = readOptionalBool(
                    target_json,
                    "skip_unchanged",
                    false,
                    target_location,
                    target.skip_unchanged,
                    result
                );
                const bool periods_ok = readHistoryPeriods(
                    target_json,
                    target_location,
                    target.strategy,
                    target.history_tiers,
                    result
                );
                const bool exclude_ok = readExcludePatterns(
                    target_json,
                    target_location,
                    target.is_directory,
                    type_ok,
                    target.exclude,
                    result
                );
                if (source_ok &&
                    retention_ok &&
                    type_ok &&
                    mode_ok &&
                    strategy_ok &&
                    cache_ok &&
                    skip_unchanged_ok &&
                    periods_ok &&
                    exclude_ok)
                {
                    target.src = std::move(source);
                    group.targets.push_back(std::move(target));
                }
            }
        }

        if (result.issues.size() != issues_before) {
            continue;
        }

        std::set<std::string> group_keys;
        bool duplicate = false;
        for (const auto& target : group.targets) {
            const std::string key =
                normalizedPathForKey(group.backup_dir) + "|" +
                normalizedPathForKey(target.src);
            if (scheduled_targets.contains(key) ||
                !group_keys.insert(key).second)
            {
                addIssue(
                    result,
                    group_location,
                    "duplicate source and backup_dir combination: " +
                        target.src.string()
                );
                duplicate = true;
            }
        }
        if (duplicate) {
            continue;
        }

        scheduled_targets.insert(group_keys.begin(), group_keys.end());
        result.groups.push_back(std::move(group));
    }

    if (jobs_it->empty()) {
        addIssue(result, "BackupJobs", "must not be empty");
    }
    return result;
}
