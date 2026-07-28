#include "ZagEditorSettings.h"
#include "ZagKpodiChanger.h"
#include "ZagProcessCommand.h"
#include "ZagEditorLog.h"

#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <thread>
#include <chrono>

#include "ContextRuntime/ContextRuntime.h"
#include "FileWatcher/FileEventDispatcher.h"
#include "scheduler/PeriodicTaskManager.h"
#include "scheduler/TaskID.h"
#include "ZagFlushPendingTask.h"
#include "MyUtils/LogFile.h"
#include "MyUtils/Encoding.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

static bool iequals(std::string a, std::string b) {
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(a.begin(), a.end(), a.begin(), lower);
    std::transform(b.begin(), b.end(), b.begin(), lower);
    return a == b;
}

static bool hasZagExt(const fs::path& p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".zag";
}

static void usage() {
    std::cout
        << "ZagEditor - fix .zag From= by EXPORT.INI\n\n"
        << "Usage:\n"
        << "  ZagEditor [--config <ini>] [--dict <path>]\n"
        << "           [--service] [--once]\n"
        << "           [--recursive|--no-recursive]\n"
        << "           [--backup|--no-backup] [--verbose]\n\n"
        << "Default (without --once): run forever and react to new *.zag files\n"
        << "via the existing FileWatcher stack (same logic as main project).\n";
}

static bool copyBak(const fs::path& src) {
    std::error_code ec;
    fs::path bak = src;
    bak += L".bak";
    fs::copy_file(src, bak, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path configPath = "zag_editor.ini";
        bool serviceMode = false;
        bool onceMode = false;
        bool verbose = false;
        bool haveOverrideRecursive = false;
        bool overrideRecursive = true;
        bool haveOverrideBackup = false;
        bool overrideBackup = true;
        fs::path overrideDict;

        std::vector<fs::path> cliPaths;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--help" || a == "-h" || a == "/?") {
                usage();
                return 0;
            }
            if (a == "--service") {
                serviceMode = true;
                continue;
            }
            if (a == "--once") {
                onceMode = true;
                continue;
            }
            if (a == "--config" && i + 1 < argc) {
                configPath = fs::path(argv[++i]);
                continue;
            }
            if (a == "--dict" && i + 1 < argc) {
                overrideDict = fs::path(argv[++i]);
                continue;
            }
            if (a == "--verbose") {
                verbose = true;
                continue;
            }
            if (a == "--recursive") {
                haveOverrideRecursive = true;
                overrideRecursive = true;
                continue;
            }
            if (a == "--no-recursive") {
                haveOverrideRecursive = true;
                overrideRecursive = false;
                continue;
            }
            if (a == "--backup") {
                haveOverrideBackup = true;
                overrideBackup = true;
                continue;
            }
            if (a == "--no-backup") {
                haveOverrideBackup = true;
                overrideBackup = false;
                continue;
            }

            cliPaths.emplace_back(fs::path(argv[i]));
        }

        auto loaded = zag_editor::loadSettingsIni(configPath);
        zag_editor::Settings s = loaded.settings;

        if (haveOverrideRecursive) s.recursive = overrideRecursive;
        if (haveOverrideBackup) s.backup = overrideBackup;
        if (!overrideDict.empty()) s.dict_path = overrideDict;

        // merge: INI input_dirs + CLI paths
        for (auto& p : cliPaths) {
            if (p.empty()) continue;
            std::error_code ec;
            fs::path abs = fs::absolute(p, ec);
            if (!ec) p = abs;
            s.input_dirs.push_back(p);
        }

        if (s.dict_path.empty()) {
            std::cerr << "ERROR: dict_path is empty. Provide it in INI (dict_path=...) or via --dict.\n";
            return 2;
        }

        if (s.input_dirs.empty()) {
            s.input_dirs.push_back(fs::path("D:\\in"));
        }

        zag_editor::ZagKpodiChanger changer(s.dict_path);
        zag_editor::MinuteThrottledLogger logger;
        logger.setTeeToConsole(!serviceMode);

        // Greeting + active settings
        std::cout << "=== ZagEditor started ===\n";
        std::cout << "mode: " << (onceMode ? "once" : (serviceMode ? "service" : "foreground")) << "\n";
        std::cout << "config: " << configPath.string() << "\n";
        std::cout << "dict_path: " << s.dict_path.string() << "\n";
        std::cout << "recursive: " << (s.recursive ? "true" : "false") << "\n";
        std::cout << "backup: " << (s.backup ? "true" : "false") << "\n";
        std::cout << "input_dirs:\n";
        for (const auto& d : s.input_dirs) std::cout << "  - " << d.string() << "\n";
        std::cout << "logs: logs/zag_editor/YYYY-MM-DD_HHMM.log (and console tee=" << (!serviceMode ? "on" : "off") << ")\n";
        std::cout << "=========================\n";

        // ------------------------------------------------------------
        // One-shot mode: scan existing files and exit.
        // ------------------------------------------------------------
        if (onceMode) {
            size_t processed = 0;
            size_t changed = 0;
            size_t skipped = 0;
            size_t errors = 0;

            auto processOneZag = [&](const fs::path& f) {
                if (!f.has_extension() || f.extension() != L".zag") { ++skipped; return; }
                if (s.backup) {
                    if (!copyBak(f)) {
                        ++errors;
                        std::cerr << "ERROR: backup failed: " << f.string() << "\n";
                        logger.logError(L"[ZagEditor] ERROR backup failed path=" + f.wstring());
                        return;
                    }
                }
                ++processed;
                bool ok = verbose ? changer.processFile(f, &std::cerr) : changer.processFile(f);
                if (ok) {
                    ++changed;
                    logger.logInfo(L"[ZagEditor] OK changed path=" + f.wstring());
                } else {
                    logger.logInfo(L"[ZagEditor] SKIP unchanged path=" + f.wstring());
                }
            };

            auto processPath = [&](const fs::path& p) {
                std::error_code ec;
                if (!fs::exists(p, ec) || ec) { ++errors; std::cerr << "ERROR: path not found: " << p.string() << "\n"; return; }
                if (fs::is_regular_file(p, ec) && !ec) { processOneZag(p); return; }
                if (fs::is_directory(p, ec) && !ec) {
                    if (s.recursive) {
                        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
                             it != fs::recursive_directory_iterator(); it.increment(ec))
                        {
                            if (ec) { ec.clear(); continue; }
                            if (it->is_regular_file(ec) && !ec) processOneZag(it->path());
                        }
                    } else {
                        for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                            if (ec) { ec.clear(); continue; }
                            if (e.is_regular_file(ec) && !ec) processOneZag(e.path());
                        }
                    }
                    return;
                }
                ++skipped;
            };

            for (const auto& d : s.input_dirs) {
                if (!d.empty()) processPath(d);
            }

            std::cout
                << "Done.\n"
                << "Dict entries: " << changer.dictSize() << "\n"
                << "Processed:    " << processed << "\n"
                << "Changed:      " << changed << "\n"
                << "Skipped:      " << skipped << "\n"
                << "Errors:       " << errors << "\n";

            return (errors > 0) ? 1 : 0;
        }

        // ------------------------------------------------------------
        // Service / foreground mode: watch using FileWatcher stack.
        // ------------------------------------------------------------
#ifdef _WIN32
        if (serviceMode) {
            // hide console window in background mode
            ShowWindow(GetConsoleWindow(), SW_HIDE);
        }
#endif

        LogFile::ensureLogsDir();

        // FileEventDispatcher expects UTF-8 strings from config-like dirs.
        std::vector<std::string> watchDirs;
        watchDirs.reserve(s.input_dirs.size());
        for (const auto& d : s.input_dirs) {
            if (d.empty()) continue;
            watchDirs.push_back(encoding::wstring_to_utf8(d.wstring()));
        }

        std::vector<std::string> ext = {"zag"};

        // Runtime threads for watcher + periodic flush.
        // 0 => auto size based on CPU.
        ContextRuntime runtime(0);
        runtime.start();

        FileEventDispatcher dispatcher(watchDirs, ext, runtime.scheduler());

        dispatcher.registerCommand(
            FileEvent::Added,
            std::make_unique<zag_editor::ZagProcessCommand>(changer, s.backup, logger)
        );

        PeriodicTaskManager<TaskId> scheduler;
        scheduler.addTask<zag_editor::ZagFlushPendingTask>(
            TaskId::FlushPendingTask,
            runtime.scheduler(),
            runtime.cpu_pool().get_executor(),
            std::chrono::seconds(2),
            dispatcher,
            logger
        );

        // keep process alive
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}

