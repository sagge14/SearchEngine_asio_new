#include "Backup/Restore/RestoreInterfaces.h"
#include "CliFormat.h"
#include "CliProgress.h"
#include "MyUtils/Encoding.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<std::wstring> commandLineArguments(
    int argc,
#ifdef _WIN32
    wchar_t* argv[]
#else
    char* argv[]
#endif
)
{
    std::vector<std::wstring> result;
    result.reserve(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index) {
#ifdef _WIN32
        result.emplace_back(argv[index]);
#else
        const std::string argument(argv[index]);
        result.emplace_back(argument.begin(), argument.end());
#endif
    }
    return result;
}

std::string narrow(const std::wstring& value)
{
    return encoding::wstring_to_utf8(value);
}

fs::path pathFromArg(const std::wstring& value)
{
    return fs::path(value);
}

std::string usage()
{
    return
        "BackupRestore - restore from mirror_history stores\n"
        "Usage:\n"
        "  BackupRestore targets  --root <backup_root>\n"
        "  BackupRestore points   --root <backup_root> --target <id|path>\n"
        "  BackupRestore files    --point <manifest.json>\n"
        "  BackupRestore show     --point <manifest.json>\n"
        "  BackupRestore plan     --point <manifest.json> [--path rel]...\n"
        "  BackupRestore verify   --point <manifest.json> [--path rel]...\n"
        "  BackupRestore restore  --point <manifest.json> --to <dir>\n"
        "                         [--overwrite] [--path rel]...\n"
        "  BackupRestore restore  --root <backup_root> --target <id>\n"
        "                         --latest --to <dir>\n"
        "                         [--overwrite] [--path rel]...\n";
}

struct Options {
    std::string command;
    fs::path root;
    std::wstring target;
    fs::path point;
    fs::path to;
    bool latest = false;
    bool overwrite = false;
    bool help = false;
    std::vector<std::string> paths;
};

bool parseOptions(
    const std::vector<std::wstring>& arguments,
    Options& options,
    std::string& error)
{
    if (arguments.size() < 2) {
        error = "command is required";
        return false;
    }

    options.command = narrow(arguments[1]);
    if (options.command == "--help" || options.command == "-h") {
        options.help = true;
        return true;
    }

    for (size_t index = 2; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        auto requireValue = [&](const wchar_t* name) -> bool {
            if (index + 1 >= arguments.size()) {
                error = narrow(name) + std::string(" requires a value");
                return false;
            }
            return true;
        };

        if (argument == L"--root") {
            if (!requireValue(L"--root")) {
                return false;
            }
            options.root = pathFromArg(arguments[++index]);
        } else if (argument == L"--target") {
            if (!requireValue(L"--target")) {
                return false;
            }
            options.target = arguments[++index];
        } else if (argument == L"--point") {
            if (!requireValue(L"--point")) {
                return false;
            }
            options.point = pathFromArg(arguments[++index]);
        } else if (argument == L"--to") {
            if (!requireValue(L"--to")) {
                return false;
            }
            options.to = pathFromArg(arguments[++index]);
        } else if (argument == L"--path") {
            if (!requireValue(L"--path")) {
                return false;
            }
            options.paths.push_back(narrow(arguments[++index]));
        } else if (argument == L"--latest") {
            options.latest = true;
        } else if (argument == L"--overwrite") {
            options.overwrite = true;
        } else if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else {
            error = "unknown argument: " + narrow(argument);
            return false;
        }
    }
    return true;
}

int runCommand(const Options& options)
{
    auto services = createMirrorHistoryRestoreServices();
    std::string error;
    CliProgress progress;

    if (options.command == "targets") {
        if (options.root.empty()) {
            std::cerr << "ERROR: --root is required\n\n" << usage();
            return 2;
        }
        const auto targets =
            services.scanner->scanRoot(options.root, error);
        if (!error.empty()) {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        printTargets(targets);
        return 0;
    }

    if (options.command == "points") {
        if (options.root.empty() || options.target.empty()) {
            std::cerr
                << "ERROR: --root and --target are required\n\n"
                << usage();
            return 2;
        }
        RestoreTargetInfo target;
        if (!services.points->findTarget(
                options.root,
                narrow(options.target),
                target,
                error))
        {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        const auto points = services.points->listPoints(target, error);
        if (!error.empty()) {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        printPoints(points);
        return 0;
    }

    auto loadPoint = [&](RestorePointInfo& point) -> bool {
        if (!options.point.empty()) {
            return services.points->loadPointFromManifest(
                options.point,
                point,
                error
            );
        }
        if (options.latest) {
            if (options.root.empty() || options.target.empty()) {
                error = "--latest requires --root and --target";
                return false;
            }
            RestoreTargetInfo target;
            if (!services.points->findTarget(
                    options.root,
                    narrow(options.target),
                    target,
                    error))
            {
                return false;
            }
            const auto points =
                services.points->listPoints(target, error);
            if (!error.empty()) {
                return false;
            }
            if (points.empty()) {
                error = "no restore points found";
                return false;
            }
            point = points.front();
            return true;
        }
        error = "--point or --latest is required";
        return false;
    };

    if (options.command == "files" || options.command == "show") {
        RestorePointInfo point;
        if (!loadPoint(point)) {
            std::cerr << "ERROR: " << error << '\n';
            return options.point.empty() ? 2 : 1;
        }
        if (options.command == "show") {
            std::cout
                << "Target:    " << point.target_id << '\n'
                << "Tier:      " << point.tier << '\n'
                << "Label:     " << point.label << '\n'
                << "Date:      " << point.date_local << ' '
                << point.time_local << '\n'
                << "Complete:  " << (point.complete ? "yes" : "no")
                << '\n'
                << "Files:     " << point.file_count << '\n'
                << "Size:      " << formatBytes(point.total_size) << '\n'
                << "Errors:    " << point.error_count << '\n'
                << "Source:    " << point.source_path << '\n'
                << "Manifest:  " << point.manifest_path.string() << '\n';
            if (options.command == "show") {
                // also list files below
            }
        }
        const auto files = services.files->listFiles(point, error);
        if (!error.empty()) {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        if (options.command == "show") {
            std::cout << '\n';
        }
        printFiles(files);
        return 0;
    }

    if (options.command == "plan") {
        RestorePointInfo point;
        if (!loadPoint(point)) {
            std::cerr << "ERROR: " << error << '\n';
            return 2;
        }
        const auto entries =
            services.planner->plan(point, options.paths, error);
        if (!error.empty()) {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        printPlan(entries);
        return 0;
    }

    if (options.command == "verify") {
        RestorePointInfo point;
        if (!loadPoint(point)) {
            std::cerr << "ERROR: " << error << '\n';
            return 2;
        }
        if (!services.verifier->verify(
                point,
                options.paths,
                &progress,
                error))
        {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        std::cout << "Verify OK\n";
        return 0;
    }

    if (options.command == "restore") {
        if (options.to.empty()) {
            std::cerr << "ERROR: --to is required\n\n" << usage();
            return 2;
        }
        RestorePointInfo point;
        if (!loadPoint(point)) {
            std::cerr << "ERROR: " << error << '\n';
            return 2;
        }
        RestoreRequest request;
        request.point = point;
        request.destination = options.to;
        request.overwrite = options.overwrite;
        request.path_filter = options.paths;
        if (!services.executor->restore(request, &progress, error)) {
            std::cerr << "ERROR: " << error << '\n';
            return 1;
        }
        std::cout
            << "Restore completed: "
            << options.to.string()
            << '\n';
        return 0;
    }

    std::cerr << "ERROR: unknown command: " << options.command << "\n\n"
              << usage();
    return 2;
}

int backupRestoreMain(
    int argc,
#ifdef _WIN32
    wchar_t* argv[]
#else
    char* argv[]
#endif
)
{
    const auto arguments = commandLineArguments(argc, argv);
    Options options;
    std::string error;
    if (!parseOptions(arguments, options, error)) {
        std::cerr << "ERROR: " << error << "\n\n" << usage();
        return 2;
    }
    if (options.help) {
        std::cout << usage();
        return 0;
    }
    return runCommand(options);
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    return backupRestoreMain(argc, argv);
}
#else
int main(int argc, char* argv[])
{
    return backupRestoreMain(argc, argv);
}
#endif
