#include "BackupTask.h"
#include <algorithm>
#include <iostream>
#include <utf8proc.h>
#include <iostream>
#include <filesystem>

namespace {
// ---------------------------------------------
//  UTF-8 → std::wstring при помощи utf8proc
// ---------------------------------------------
    static std::wstring utf8_to_wstring(const std::string& s)
    {
        std::wstring out;
        out.reserve(s.size());
        const utf8proc_uint8_t* data = reinterpret_cast<const utf8proc_uint8_t*>(s.data());
        const utf8proc_ssize_t len   = static_cast<utf8proc_ssize_t>(s.size());

        for (utf8proc_ssize_t i = 0; i < len; ) {
            utf8proc_int32_t cp;
            utf8proc_ssize_t n = utf8proc_iterate(data + i, len - i, &cp);
            if (n < 0) { // невалидная последовательность → U+FFFD
                cp = 0xFFFD;
                n  = 1;
            }
#if WCHAR_MAX >= 0x10FFFF // Linux / macOS (UTF-32)
            out.push_back(static_cast<wchar_t>(cp));
#else                    // Windows (UTF-16)
            if (cp <= 0xFFFF) {
                out.push_back(static_cast<wchar_t>(cp));
            } else { // суррогатная пара
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>((cp >> 10)   + 0xD800));
                out.push_back(static_cast<wchar_t>((cp & 0x3FF) + 0xDC00));
            }
#endif
            i += n;
        }
        return out;
    }

// wide-stream logging c utf8proc-конвертацией
    void logError(const char* what,
                  const fs::path& p,
                  const std::error_code& ec)
    {
        const std::wstring what_w = utf8_to_wstring(what);
        const std::wstring msg_w  = utf8_to_wstring(ec.message());

        std::wcerr << L"[ERROR] " << what_w
                   << L" \""    << p.wstring()
                   << L"\": ("  << ec.value() << L") "
                   << msg_w << L'\n';
    }
} // anonymous namespace


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

void BackupTask::backupDirectory(const BackupTarget& target, const std::string& time_str)
{
    std::error_code ec;

    // 1. Быстрая проверка – без исключений.
    if (!fs::exists(target.src, ec) || ec) {
        logError("src exists", target.src, ec);
        return;
    }

    fs::path base_backup = backup_root_ / (target.src.filename().string() + "_backup");

    // 2. Первая «базовая» копия.
    if (!fs::exists(base_backup, ec) || ec) {
        fs::create_directories(base_backup, ec);
        if (ec) { logError("create_directories", base_backup, ec); return; }

        fs::copy(target.src, base_backup,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing |
                 fs::copy_options::skip_symlinks, ec);
        if (ec) { logError("initial copy", target.src, ec); }
        else    { std::cout << "Initial backup dir: " << base_backup << '\n'; }
        return;
    }

    // 3. Ищем изменённые файлы.
    std::vector<fs::path> changed;
    auto opts = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(target.src, opts, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { logError("iter", (it != end ? it->path() : target.src), ec); ec.clear(); continue; }
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }

        fs::path rel = fs::relative(it->path(), target.src, ec);
        if (ec) { logError("relative", it->path(), ec); ec.clear(); continue; }

        std::error_code ec2;
        fs::path base_file = base_backup / rel;
        auto src_time  = fs::last_write_time(it->path(), ec2);
        auto base_time = fs::exists(base_file, ec2) ? fs::last_write_time(base_file, ec2)
                                                    : fs::file_time_type::min();
        if (src_time > base_time)
            changed.push_back(rel);
    }

    if (changed.empty()) return;

    // 4. Создаём версионную папку.
    fs::path version_dir = backup_root_ / (
            target.src.parent_path().filename().string() + "_" +
            target.src.filename().string() + "_" + time_str);
    fs::create_directories(version_dir, ec);
    if (ec) { logError("create_directories", version_dir, ec); return; }

    // 5. Копируем изменённые файлы и обновляем «базу».
    for (const auto& rel : changed) {
        fs::create_directories((version_dir / rel).parent_path(), ec);
        if (ec) { logError("mkdir version", version_dir / rel, ec); ec.clear(); }

        fs::copy_file(target.src / rel, version_dir / rel,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) { logError("copy version", rel, ec); ec.clear(); }

        fs::create_directories((base_backup / rel).parent_path(), ec);
        if (ec) { logError("mkdir base", base_backup / rel, ec); ec.clear(); }

        fs::copy_file(target.src / rel, base_backup / rel,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) { logError("copy base", rel, ec); ec.clear(); }
    }

    cleanupOldBackups(target, true);
    std::cout << "Backup dir: " << target.src << " -> " << version_dir << '\n';
}

void BackupTask::backupFile(const BackupTarget& target, const std::string& time_str)
{
    std::error_code ec;

    // 1. Проверка наличия исходного файла.
    if (!fs::exists(target.src, ec) || !fs::is_regular_file(target.src, ec) || ec) {
        logError("src file", target.src, ec);
        return;
    }

    // 2. Каталог с «базовой» копией.
    const std::string fname = target.src.parent_path().filename().string() + "_" +
                              target.src.filename().string() + "_base";
    fs::path base_backup = backup_root_ / fname;
    fs::create_directories(base_backup, ec);
    if (ec) { logError("create_directories", base_backup, ec); return; }

    fs::path base_file = base_backup / target.src.filename();

    // 3. Если базовой копии ещё нет — просто создаём и выходим.
    if (!fs::exists(base_file, ec) || ec) {
        fs::copy_file(target.src, base_file, fs::copy_options::overwrite_existing, ec);
        if (ec) { logError("init copy", base_file, ec); }
        else    { std::cout << "Initial backup file: " << base_file << '\n'; }
        return;
    }

    // 4. Сравниваем времена изменений.
    auto src_time  = fs::last_write_time(target.src, ec);
    if (ec) { logError("lwt src", target.src, ec); return; }
    auto base_time = fs::last_write_time(base_file, ec);
    if (ec) { logError("lwt base", base_file, ec); return; }

    if (src_time <= base_time)
        return; // изменений нет

    // 5. Создаём версионную папку.
    const std::string vname = target.src.parent_path().filename().string() + "_" +
                              target.src.filename().string() + "_" + time_str;
    fs::path version_dir = backup_root_ / vname;
    fs::create_directories(version_dir, ec);
    if (ec) { logError("create version dir", version_dir, ec); return; }

    fs::path dst = version_dir / target.src.filename();

    // 6. Копируем в версию.
    fs::copy_file(target.src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { logError("copy version", dst, ec); ec.clear(); }

    // 7. Обновляем «базу».
    fs::copy_file(target.src, base_file, fs::copy_options::overwrite_existing, ec);
    if (ec) { logError("update base", base_file, ec); ec.clear(); }

    // 8. Чистим старые версии.
    cleanupOldBackups(target, false);

    std::cout << "Backup file: " << target.src << " -> " << dst << '\n';
}


void BackupTask::cleanupOldBackups(const BackupTarget& target, bool is_dir)
{
    std::error_code ec;
    std::vector<fs::directory_entry> versions;

    // Общий префикс для версий.
    const std::string prefix = target.src.parent_path().filename().string() + "_" +
                               target.src.filename().string() + "_";

    // Имя базовой копии.
    const std::string base_backup_name = is_dir
                                         ? target.src.filename().string() + "_backup"
                                         : target.src.parent_path().filename().string() + "_" + target.src.filename().string() + "_base";

    // Обход каталога с пропуском denied.
    for (fs::directory_iterator it(backup_root_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { logError("iter cleanup", backup_root_, ec); ec.clear(); continue; }
        if (!it->is_directory(ec) || ec) { ec.clear(); continue; }

        const std::string fname = it->path().filename().string();
        if (fname.starts_with(prefix) && fname != base_backup_name)
            versions.push_back(*it);
    }

    if (versions.size() <= target.max_versions)
        return;

    std::sort(versions.begin(), versions.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b)
              {
                  return a.path().filename().string() < b.path().filename().string();
              });

    const size_t to_remove = versions.size() - target.max_versions;
    for (size_t i = 0; i < to_remove; ++i) {
        fs::remove_all(versions[i], ec);
        if (ec) { logError("remove backup", versions[i].path(), ec); ec.clear(); }
    }
}

