#pragma once

#include "Backup/Restore/RestoreTypes.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

inline std::string formatBytes(std::uint64_t bytes)
{
    std::ostringstream stream;
    if (bytes < 1024) {
        stream << bytes << " B";
    } else if (bytes < 1024ull * 1024) {
        stream << std::fixed << std::setprecision(1)
               << (bytes / 1024.0) << " KB";
    } else if (bytes < 1024ull * 1024 * 1024) {
        stream << std::fixed << std::setprecision(1)
               << (bytes / (1024.0 * 1024.0)) << " MB";
    } else {
        stream << std::fixed << std::setprecision(2)
               << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    return stream.str();
}

inline const char* resolveStatusText(RestoreResolveStatus status)
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

inline void printTargets(const std::vector<RestoreTargetInfo>& targets)
{
    std::cout
        << std::left
        << std::setw(24) << "DISPLAY"
        << std::setw(40) << "ID"
        << std::setw(10) << "CURRENT"
        << std::setw(8) << "FILES"
        << std::setw(12) << "SIZE"
        << '\n';
    for (const auto& target : targets) {
        std::cout
            << std::left
            << std::setw(24) << target.display_name.substr(0, 23)
            << std::setw(40) << target.id.substr(0, 39)
            << std::setw(10) << (target.has_current ? "yes" : "no")
            << std::setw(8) << target.file_count_current
            << std::setw(12) << formatBytes(target.total_size_current)
            << '\n';
    }
}

inline void printPoints(const std::vector<RestorePointInfo>& points)
{
    std::cout
        << std::left
        << std::setw(12) << "DATE"
        << std::setw(10) << "TIME"
        << std::setw(14) << "TIER"
        << std::setw(18) << "LABEL"
        << std::setw(10) << "COMPLETE"
        << std::setw(8) << "FILES"
        << std::setw(12) << "SIZE"
        << '\n';
    for (const auto& point : points) {
        std::cout
            << std::left
            << std::setw(12)
            << (point.date_local.empty() ? "-" : point.date_local)
            << std::setw(10)
            << (point.time_local.empty() ? "-" : point.time_local)
            << std::setw(14) << point.tier.substr(0, 13)
            << std::setw(18) << point.label.substr(0, 17)
            << std::setw(10) << (point.complete ? "yes" : "NO")
            << std::setw(8) << point.file_count
            << std::setw(12) << formatBytes(point.total_size)
            << '\n';
    }
}

inline void printFiles(const std::vector<RestoreFileInfo>& files)
{
    std::cout
        << std::left
        << std::setw(40) << "PATH"
        << std::setw(12) << "SIZE"
        << std::setw(10) << "STATUS"
        << std::setw(20) << "CAPTURED"
        << std::setw(12) << "METHOD"
        << '\n';
    for (const auto& file : files) {
        std::cout
            << std::left
            << std::setw(40) << file.relative_path.substr(0, 39)
            << std::setw(12) << formatBytes(file.size)
            << std::setw(10) << resolveStatusText(file.resolve_status)
            << std::setw(20)
            << (file.captured_at.empty()
                    ? "-"
                    : file.captured_at.substr(0, 19))
            << std::setw(12)
            << (file.method.empty() ? "-" : file.method.substr(0, 11))
            << '\n';
    }
}

inline void printPlan(const std::vector<RestorePlanEntry>& entries)
{
    std::cout
        << std::left
        << std::setw(40) << "PATH"
        << std::setw(12) << "SIZE"
        << std::setw(10) << "SOURCE"
        << "RESOLVED\n";
    std::uint64_t total = 0;
    size_t missing = 0;
    for (const auto& entry : entries) {
        total += entry.size;
        if (entry.source == RestorePlanSource::Missing) {
            ++missing;
        }
        std::cout
            << std::left
            << std::setw(40) << entry.path.substr(0, 39)
            << std::setw(12) << formatBytes(entry.size)
            << std::setw(10) << entry.status
            << entry.source_path.string()
            << '\n';
    }
    std::cout
        << "Entries: " << entries.size()
        << "  Missing: " << missing
        << "  Total: " << formatBytes(total)
        << '\n';
}
