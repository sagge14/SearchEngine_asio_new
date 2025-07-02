#include "BackupTask.h"
#include <algorithm>
#include <iostream>

BackupTask::BackupTask(boost::asio::io_context& io, std::chrono::seconds period,
                       const fs::path& backup_root, const std::vector<BackupTarget>& targets)
        : AbstractScheduledTask(io, period),
          backup_root_(backup_root),
          targets_(targets) {}

void BackupTask::runTask() {
    auto now = std::chrono::system_clock::now();
    auto time_str = getTimeString(now);

    for (auto& target : targets_) {
        if (target.is_directory) {
            backupDirectory(target, time_str);
        } else {
            backupFile(target, time_str);
        }
    }
}

std::string BackupTask::getTimeString(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
    return buf;
}

void BackupTask::backupDirectory(const BackupTarget& target, const std::string& time_str) {

    if (!fs::exists(target.src)) {
        std::cerr << "Backup: Source directory not found: " << target.src << std::endl;
        return;
    }

    fs::path base_backup = backup_root_ / (target.src.filename().string() + "_backup");

    if (!fs::exists(base_backup)) {
        fs::create_directories(base_backup);
        fs::copy(target.src, base_backup, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        std::cout << "Initial backup dir: " << base_backup << std::endl;
        return;
    }

    std::vector<fs::path> changed_files;
    for (auto& entry : fs::recursive_directory_iterator(target.src)) {
        if (!entry.is_regular_file()) continue;
        fs::path rel = fs::relative(entry.path(), target.src);
        fs::path base_file = base_backup / rel;
        if (!fs::exists(base_file) || fs::last_write_time(entry.path()) > fs::last_write_time(base_file)) {
            changed_files.push_back(rel);
        }
    }
    if (changed_files.empty()) return;

    fs::path version_dir = backup_root_ / (target.src.filename().string() + "_" + time_str);
    fs::create_directories(version_dir);

    for (const auto& rel : changed_files) {
        fs::create_directories((version_dir / rel).parent_path());
        fs::copy_file(target.src / rel, version_dir / rel, fs::copy_options::overwrite_existing);
        fs::create_directories((base_backup / rel).parent_path());
        fs::copy_file(target.src / rel, base_backup / rel, fs::copy_options::overwrite_existing);
    }

    cleanupOldBackups(target, true);
    std::cout << "Backup dir: " << target.src << " -> " << version_dir << std::endl;
}

void BackupTask::backupFile(const BackupTarget& target, const std::string& time_str) {
    if (!fs::exists(target.src) || !fs::is_regular_file(target.src)) {
        std::cerr << "Backup: Source file not found: " << target.src << std::endl;
        return;
    }
    fs::path base_backup = backup_root_ / (target.src.filename().string() + "_backup");
    fs::create_directories(base_backup.parent_path());

    try {
        if (!fs::exists(base_backup / target.src.filename())) {
            fs::copy_file(target.src, base_backup / target.src.filename(), fs::copy_options::overwrite_existing);
            std::cout << "Initial backup file: " << (base_backup / target.src.filename()) << std::endl;
            return;
        }

        if (fs::last_write_time(target.src) <= fs::last_write_time(base_backup / target.src.filename()))
            return;

        fs::path version_dir = backup_root_ / (target.src.filename().string() + "_" + time_str);
        fs::create_directories(version_dir);

        // Вот тут главное:
        fs::path dst = version_dir / target.src.filename();
        fs::create_directories(dst.parent_path()); // <-- создаём папку для файла!
        fs::copy_file(target.src, dst, fs::copy_options::overwrite_existing);

        fs::copy_file(target.src, base_backup / target.src.filename(), fs::copy_options::overwrite_existing);

        cleanupOldBackups(target, false);
        std::cout << "Backup file: " << target.src << " -> " << dst << std::endl;
    }
    catch (const std::exception& ex) {
        std::cerr << "Backup error (file): " << ex.what() << std::endl;
    }
}


void BackupTask::cleanupOldBackups(const BackupTarget& target, bool is_dir) {
    std::vector<fs::directory_entry> versions;
    std::string prefix = target.src.filename().string() + "_";
    for (auto& entry : fs::directory_iterator(backup_root_)) {
        if (!entry.is_directory()) continue;
        auto fname = entry.path().filename().string();
        if (fname.find(prefix) == 0 && fname != (target.src.filename().string() + "_backup"))
            versions.push_back(entry);
    }
    if (versions.size() > target.max_versions) {
        std::sort(versions.begin(), versions.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b) {
                      return a.path().filename().string() < b.path().filename().string();
                  });
        for (size_t i = 0; i < versions.size() - target.max_versions; ++i)
            fs::remove_all(versions[i]);
    }
}
