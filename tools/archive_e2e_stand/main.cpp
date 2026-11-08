#include "StandBuilder.h"

#include "MyUtils/Encoding.h"

#include <Windows.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

bool isConsoleHandle(HANDLE handle)
{
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(handle, &mode) != FALSE;
}

void writeText(DWORD standardHandle, std::wstring_view text)
{
    const HANDLE handle = GetStdHandle(standardHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || text.empty())
        return;

    if (isConsoleHandle(handle)) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                text.size() - offset, 16384));
            DWORD written = 0;
            if (!WriteConsoleW(handle, text.data() + offset, chunk, &written, nullptr) ||
                written == 0)
            {
                return;
            }
            offset += written;
        }
        return;
    }

    const std::string utf8 = encoding::wstring_to_utf8(std::wstring(text));
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            utf8.size() - offset, 16384));
        DWORD written = 0;
        if (!WriteFile(handle, utf8.data() + offset, chunk, &written, nullptr) ||
            written == 0)
        {
            return;
        }
        offset += written;
    }
}

template <typename... Parts>
std::wstring compose(Parts&&... parts)
{
    std::wostringstream stream;
    (stream << ... << std::forward<Parts>(parts));
    return stream.str();
}

template <typename... Parts>
void output(Parts&&... parts)
{
    writeText(
        STD_OUTPUT_HANDLE,
        compose(std::forward<Parts>(parts)...));
}

template <typename... Parts>
void outputError(Parts&&... parts)
{
    writeText(
        STD_ERROR_HANDLE,
        compose(std::forward<Parts>(parts)...));
}

fs::path currentExecutable()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        throw std::runtime_error("cannot resolve helper executable path");
    buffer.resize(length);
    return fs::path(buffer);
}

std::optional<std::wstring> option(
    const std::vector<std::wstring>& arguments,
    const std::wstring& name)
{
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name)
            return arguments[index + 1];
    }
    return std::nullopt;
}

void printSummary(const searchengine_archive_e2e::StandSummary& summary)
{
    output(
        L"Стенд проверен: ", summary.root.wstring(),
        L"\nГод: ", summary.year,
        L"\nSQLite-баз: ", summary.databaseCount,
        L"\nСтрок телеграмм: ", summary.telegramRowCount,
        L"\nУникальных телеграмм: ", summary.uniqueTelegramCount,
        L"\nПриложений: ", summary.attachmentCount,
        L"\nЗаписей выдачи F12: ", summary.f12WayRowCount,
        L"\nБайт данных: ", summary.generatedBytes, L'\n');
}

void printWorkstationSummary(
    const searchengine_archive_e2e::WorkstationStandLayout& layout)
{
    output(
        L"Стенд развернут в раскладку рабочей машины.",
        L"\nСлужба: ", layout.serviceName,
        L"\nПрограмма: ", layout.binDirectory.wstring(),
        L"\nНастройки: ", layout.dataDirectory.wstring(),
        L"\nТом данных: ", layout.dataVolumeRoot.wstring(),
        L"\nМесячные базы PRM: ", layout.prmMonthlyDirectory.wstring(),
        L"\nМесячные базы PRD: ", layout.prdMonthlyDirectory.wstring(),
        L"\nTLG: ", layout.tlgDirectory.wstring(),
        L"\nГодовая база F12: ",
        (layout.f12Directory / (std::to_wstring(layout.year) + L".db")).wstring(),
        L'\n');
}

void printUsage()
{
    output(
        L"SearchEngineArchiveE2EStand generate --root <folder> "
        L"--settings-template <Settings.json> [--year 2026] [--records 10]\n"
        L"SearchEngineArchiveE2EStand generate-service-archive "
        L"--root <output-folder> --deployment-root <target-folder> "
        L"--restore-root <target-folder> --program-template <app-folder> "
        L"--settings-template <Settings.json> --service-name <name> "
        L"[--preparer-template <helper.exe>] "
        L"[--installer-template <portable-package>] "
        L"[--port 25027] [--year 2026] [--records 10]\n"
        L"SearchEngineArchiveE2EStand prepare-service-archive "
        L"--root <unpacked-folder>\n"
        L"SearchEngineArchiveE2EStand deploy-workstation-stand "
        L"--root <unpacked-folder> --data-volume-root <D:\\> "
        L"--program-files-root <folder> --program-data-root <folder>\n"
        L"SearchEngineArchiveE2EStand verify --root <folder>\n");
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    try {
        if (argc < 2) {
            printUsage();
            return 2;
        }
        std::vector<std::wstring> arguments;
        for (int index = 2; index < argc; ++index)
            arguments.emplace_back(argv[index]);

        const std::wstring command = argv[1];
        const auto root = option(arguments, L"--root");
        if (!root) {
            printUsage();
            return 2;
        }

        if (command == L"generate" || command == L"generate-service-archive") {
            const auto settings = option(arguments, L"--settings-template");
            if (!settings) {
                printUsage();
                return 2;
            }
            searchengine_archive_e2e::StandOptions options;
            options.root = *root;
            options.settingsTemplate = *settings;
            if (const auto year = option(arguments, L"--year"))
                options.year = std::stoi(*year);
            if (const auto records = option(arguments, L"--records"))
                options.recordsPerMonth = std::stoi(*records);
            if (command == L"generate") {
                printSummary(searchengine_archive_e2e::generateStand(options));
                return 0;
            }

            const auto deploymentRoot = option(arguments, L"--deployment-root");
            const auto restoreRoot = option(arguments, L"--restore-root");
            const auto programTemplate = option(arguments, L"--program-template");
            const auto serviceName = option(arguments, L"--service-name");
            if (!deploymentRoot || !restoreRoot || !programTemplate ||
                !serviceName)
            {
                printUsage();
                return 2;
            }
            searchengine_archive_e2e::ServiceArchiveStandOptions archiveOptions;
            archiveOptions.stand = std::move(options);
            archiveOptions.deploymentRoot = *deploymentRoot;
            archiveOptions.restoreRoot = *restoreRoot;
            archiveOptions.programTemplate = *programTemplate;
            archiveOptions.preparerTemplate = option(
                arguments, L"--preparer-template").value_or(
                    currentExecutable().wstring());
            archiveOptions.installerTemplate = option(
                arguments, L"--installer-template").value_or(L"");
            archiveOptions.serviceName = *serviceName;
            if (const auto port = option(arguments, L"--port"))
                archiveOptions.port = std::stoi(*port);
            printSummary(
                searchengine_archive_e2e::generateServiceArchiveStand(
                    archiveOptions));
            return 0;
        }
        if (command == L"prepare-service-archive") {
            printSummary(
                searchengine_archive_e2e::prepareServiceArchiveStand(*root));
            return 0;
        }
        if (command == L"deploy-workstation-stand") {
            const auto dataVolume = option(arguments, L"--data-volume-root");
            const auto programFiles = option(arguments, L"--program-files-root");
            const auto programData = option(arguments, L"--program-data-root");
            if (!dataVolume || !programFiles || !programData) {
                printUsage();
                return 2;
            }
            searchengine_archive_e2e::WorkstationStandOptions options;
            options.root = *root;
            options.dataVolumeRoot = *dataVolume;
            options.programFilesRoot = *programFiles;
            options.programDataRoot = *programData;
            printWorkstationSummary(
                searchengine_archive_e2e::deployWorkstationStand(options));
            return 0;
        }
        if (command == L"verify") {
            printSummary(searchengine_archive_e2e::verifyStand(*root));
            return 0;
        }

        printUsage();
        return 2;
    } catch (const std::exception& error) {
        outputError(
            L"ERROR: ", encoding::utf8_to_wstring(error.what()), L'\n');
        return 1;
    }
}
