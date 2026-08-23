#include "ArchiveCore.h"
#include "InteractivePaths.h"
#include "ServiceArchive.h"

#include "MyUtils/Encoding.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using searchengine_archive::YearMoveOptions;
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

std::wstring readLine()
{
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (isConsoleHandle(handle)) {
        std::wstring value;
        wchar_t buffer[256];
        for (;;) {
            DWORD read = 0;
            if (!ReadConsoleW(
                    handle, buffer,
                    static_cast<DWORD>(std::size(buffer)), &read, nullptr))
            {
                throw std::runtime_error("cannot read from Windows console");
            }
            if (read == 0)
                break;
            value.append(buffer, buffer + read);
            const auto end = value.find_first_of(L"\r\n");
            if (end != std::wstring::npos) {
                value.resize(end);
                break;
            }
        }
        return value;
    }

    std::string utf8;
    std::getline(std::cin, utf8);
    if (!utf8.empty() && utf8.back() == '\r')
        utf8.pop_back();
    return encoding::utf8_to_wstring(utf8);
}

std::optional<std::wstring> option(
    const std::vector<std::wstring>& args,
    const std::wstring& name)
{
    for (std::size_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == name)
            return args[index + 1];
    }
    return std::nullopt;
}

bool hasFlag(const std::vector<std::wstring>& args, const std::wstring& name)
{
    return std::find(args.begin(), args.end(), name) != args.end();
}

std::wstring prompt(const wchar_t* text, const std::wstring& defaultValue = {})
{
    output(text);
    if (!defaultValue.empty())
        output(L" [", defaultValue, L']');
    output(L": ");
    const std::wstring value = readLine();
    return value.empty() ? defaultValue : value;
}

bool confirm(const wchar_t* text)
{
    output(text, L" [y/N]: ");
    const std::wstring value = readLine();
    return value == L"y" || value == L"Y" || value == L"д" || value == L"Д";
}

fs::path promptDirectory(
    const wchar_t* text,
    const fs::path& suggestedDirectory = {},
    bool allowDisable = false)
{
    for (;;) {
        output(text);
        if (!suggestedDirectory.empty())
            output(L" [", suggestedDirectory.wstring(), L']');
        output(
            allowDisable
                ? L"\n  Enter = выбрать папку, - = отключить, либо введите путь: "
                : L"\n  Enter = выбрать папку, либо введите путь: ");
        const std::wstring input = readLine();
        try {
            const auto result = searchengine_archive::resolveDirectoryInput(
                input,
                suggestedDirectory,
                allowDisable,
                searchengine_archive::pickFolderWindows7);
            if (result.action ==
                searchengine_archive::DirectoryInputAction::Selected)
            {
                return result.path;
            }
            if (result.action ==
                searchengine_archive::DirectoryInputAction::Disabled)
            {
                return {};
            }
            output(L"Выбор папки отменён. Укажите путь ещё раз.\n");
        } catch (const std::invalid_argument&) {
            output(L"Этот каталог обязателен; '-' здесь использовать нельзя.\n");
        }
    }
}

void printWarnings(const std::vector<std::string>& warnings)
{
    for (const auto& warning : warnings) {
        output(
            L"ПРЕДУПРЕЖДЕНИЕ: ",
            encoding::utf8_to_wstring(warning), L'\n');
    }
    if (!warnings.empty())
        output(L'\n');
}

void printPlan(const searchengine_archive::YearMovePlan& plan)
{
    printWarnings(plan.warnings);
    output(
        L"\nГод: ", plan.options.year,
        L"\nНазначение: ", plan.finalDirectory.wstring(),
        L"\nМесячных баз: ", plan.databases.size(),
        L"\n\nКаталоги верхнего уровня:\n");
    for (const auto& mapping : plan.mappings) {
        output(
            L"  ", mapping.source.wstring(),
            L"\n    -> ", mapping.target.wstring(), L'\n');
        if (searchengine_archive::isTlgArchiveRoot(mapping.source)) {
            output(
                L"    Пропускаются: OUT и папки годов, не равные ",
                plan.options.year, L".\n");
        }
    }
    output(L"\nИсходные файлы и базы сейчас удаляться не будут.\n");
}

void printServicePlan(const searchengine_archive::ServiceArchivePlan& plan)
{
    printWarnings(plan.warnings);
    output(
        L"\nСлужба: ", plan.service.serviceName,
        L"\nГод: ", plan.year,
        L"\nТекущий EXE: ", plan.service.executable.wstring(),
        L"\nТекущие данные: ", plan.service.dataDirectory.wstring(),
        L"\nАрхив: ", plan.finalDirectory.wstring(),
        L"\nНовый ImagePath: ", plan.archivedImagePath,
        L"\nМесячных баз: ", plan.monthlyDatabases.size(),
        L"\n\nПереносимые каталоги:\n");
    for (const auto& mapping : plan.mappings) {
        output(
            L"  ", mapping.source.wstring(),
            L"\n    -> ", mapping.target.wstring(), L'\n');
        if (searchengine_archive::isTlgArchiveRoot(mapping.source)) {
            output(
                L"    Пропускаются: OUT и папки годов, не равные ",
                plan.year, L".\n");
        }
    }
    output(
        L"\nСлужба будет остановлена только на время согласованного "
        L"копирования и переключения. Исходники пока не удаляются.\n");
}

void printServiceRestorePlan(
    const searchengine_archive::ServiceRestorePlan& plan)
{
    output(
        L"\nАрхив: ", plan.archiveDirectory.wstring(),
        L"\nРежим: ",
        plan.mode == searchengine_archive::ServiceRestoreMode::OriginalLocations
            ? L"возврат в записанные исходные места 1:1"
            : L"переносимый возврат под выбранный каталог",
        L"\nНовый ImagePath: ", plan.restoredImagePath,
        L"\nМесячных баз: ", plan.monthlyDatabases.size(),
        L"\n\nВосстанавливаемые каталоги:\n");
    if (plan.mode == searchengine_archive::ServiceRestoreMode::SelectedRoot) {
        output(
            L"Целевой корневой каталог: ",
            plan.restoreRoot.wstring(), L'\n');
    }
    for (const auto& mapping : plan.mappings) {
        output(
            L"  ", mapping.source.wstring(),
            L"\n    -> ", mapping.target.wstring(), L'\n');
    }
    if (plan.mode == searchengine_archive::ServiceRestoreMode::OriginalLocations) {
        output(
            L"\nПрограмма и данные службы возвращаются строго в исходные "
            L"места. Каталоги индексируемого содержимого безопасно "
            L"дополняются без перезаписи существующих файлов.\n");
    } else {
        output(
            L"\nСлияние и перезапись запрещены. Если в выбранном корне "
            L"уже есть хотя бы один целевой каталог, операция не начнётся. "
            L"Сам корень диска использовать можно.\n");
    }
}

YearMoveOptions interactiveYearOptions()
{
    YearMoveOptions options;
    options.year = std::stoi(prompt(L"Архивируемый год"));
    options.prmMonthlyDirectory = promptDirectory(
        L"Каталог месячных PRM-баз",
        L"D:\\BASES\\METH_BASES",
        true);
    options.prdMonthlyDirectory = promptDirectory(
        L"Каталог месячных PRD-баз",
        L"D:\\BASES_PRD\\METH_BASES",
        true);
    options.archiveRoot = promptDirectory(L"Корневой каталог архива");
    return options;
}

int runYearOnly(const YearMoveOptions& options, bool assumeYes)
{
    const auto plan = searchengine_archive::planYearMove(options);
    printPlan(plan);
    if (!assumeYes && !confirm(L"Скопировать год в staging и опубликовать архив?")) {
        output(L"Операция отменена.\n");
        return 2;
    }
    const auto result = searchengine_archive::executeYearMove(
        plan,
        [](const std::wstring& message) { output(message, L'\n'); });
    if (!result.ok) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(result.message), L'\n');
        return 1;
    }
    output(
        L"\nАрхив опубликован: ", result.finalDirectory.wstring(),
        L"\nФайлов: ", result.copiedFiles,
        L", байт: ", result.copiedBytes,
        L"\n\nПроверьте архив вручную. Исходники сохранены.\n",
        L"Для удаления подтверждённых исходников запустите утилиту снова "
        L"с командой --cleanup-year и путём архива.\n");
    return 0;
}

int runCleanup(const fs::path& finalDirectory, bool deleteDatabases, bool assumeYes)
{
    if (!assumeYes && !confirm(
            L"Удалить только неизменившиеся исходные файлы из manifest?")) {
        output(L"Удаление отменено. Архив остаётся рабочим.\n");
        return 2;
    }
    if (deleteDatabases && !assumeYes && !confirm(
            L"Также удалить исходные месячные базы, кроме декабрьской PRD?")) {
        deleteDatabases = false;
    }
    const auto result = searchengine_archive::cleanupYearMoveFiles(
        finalDirectory,
        deleteDatabases,
        [](const std::wstring& message) { output(message, L'\n'); });
    if (!result.ok) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(result.message), L'\n');
        return 1;
    }
    output(L"Очистка исходников завершена.\n");
    return 0;
}

int runServiceCleanup(
    const fs::path& archiveDirectory,
    bool deleteMonthlyDatabases,
    bool assumeYes);

int runServiceArchive(
    const searchengine_archive::ServiceArchiveOptions& options,
    bool assumeYes)
{
    const auto plan = searchengine_archive::planServiceArchive(options);
    printServicePlan(plan);
    if (!assumeYes && !confirm(
            L"Остановить службу, создать проверенную архивную копию и переключить SCM?"))
    {
        output(L"Операция отменена.\n");
        return 2;
    }
    if (!assumeYes && !confirm(
            L"Подтверждаете, что архив расположен на надёжном локальном диске?"))
    {
        output(L"Операция отменена.\n");
        return 2;
    }
    const auto result = searchengine_archive::executeServiceArchive(
        plan,
        [](const std::wstring& message) { output(message, L'\n'); });
    if (!result.ok) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(result.message), L'\n');
        return 1;
    }
    output(
        L"\nСлужба запущена из архива: ",
        result.archiveDirectory.wstring(),
        L"\nРежим сервера: archive. Автоматическая индексация отключена.\n",
        L"Ручное обновление разрешено только пользователю admin, "
        L"подключённому с 127.0.0.1.\n",
        L"\nИсходное место сохранено. Проверьте поиск вручную, затем "
        L"вернитесь в это окно.\n");
    if (!assumeYes) {
        if (confirm(
                L"Ручная проверка завершена. Перейти к этапу очистки старого места?"))
        {
            return runServiceCleanup(
                result.archiveDirectory,
                true,
                false);
        }
        output(
            L"Старое место сохранено. Очистку можно выполнить позже через "
            L"пункт 3 меню операций со службой.\n");
    }
    return 0;
}

int runServiceRestore(
    const fs::path& archiveDirectory,
    std::optional<searchengine_archive::ServiceRestoreMode> restoreMode,
    std::optional<fs::path> restoreRoot,
    bool assumeYes)
{
    using searchengine_archive::ServiceRestoreMode;
    if (!restoreMode) {
        if (restoreRoot) {
            restoreMode = ServiceRestoreMode::SelectedRoot;
        } else if (assumeYes) {
            throw std::runtime_error(
                "--restore-original or --restore-root is required together "
                "with --yes");
        } else {
            output(
                L"\nВыберите способ разморозки:\n"
                L"  1 - Вернуть в записанные исходные места 1:1 "
                L"(рекомендуется)\n"
                L"  2 - Восстановить всё под выбранный каталог "
                L"(переносимый режим)\n"
                L"  0 - Назад\n"
                L"Ваш выбор: ");
            const std::wstring choice = readLine();
            if (choice == L"0" || choice.empty())
                return 0;
            if (choice == L"1")
                restoreMode = ServiceRestoreMode::OriginalLocations;
            else if (choice == L"2")
                restoreMode = ServiceRestoreMode::SelectedRoot;
            else
                throw std::runtime_error("unknown restore mode choice");
        }
    }
    if (*restoreMode == ServiceRestoreMode::OriginalLocations && restoreRoot) {
        throw std::runtime_error(
            "--restore-original and --restore-root cannot be used together");
    }
    if (*restoreMode == ServiceRestoreMode::SelectedRoot && !restoreRoot) {
        if (assumeYes)
            throw std::runtime_error("--restore-root is required with --yes");
        output(
            L"\nВарианты места восстановления:\n"
            L"  D: или D:\\    — прямо в корень диска D:\\\n"
            L"  D:\\StandV3    — внутрь отдельной папки\n"
            L"  Enter          — выбрать каталог в окне\n"
            L"Существующие целевые каталоги не объединяются и не "
            L"перезаписываются.\n\n");
        restoreRoot = promptDirectory(
            L"Новый корневой каталог восстановленной службы");
    }
    const auto plan = *restoreMode == ServiceRestoreMode::OriginalLocations
        ? searchengine_archive::planServiceRestoreOriginalLocations(
            archiveDirectory)
        : searchengine_archive::planServiceRestore(
            archiveDirectory, *restoreRoot);
    printServiceRestorePlan(plan);
    if (!assumeYes && !confirm(
            *restoreMode == ServiceRestoreMode::OriginalLocations
                ? L"Остановить архивную службу, вернуть все данные в "
                  L"исходные места и разморозить сервер?"
                : L"Остановить архивную службу, восстановить её под "
                  L"выбранный каталог и разморозить сервер?"))
    {
        output(L"Операция отменена.\n");
        return 2;
    }
    const auto progress =
        [](const std::wstring& message) { output(message, L'\n'); };
    const auto result = *restoreMode == ServiceRestoreMode::OriginalLocations
        ? searchengine_archive::restoreServiceArchiveOriginalLocations(
            archiveDirectory, progress)
        : searchengine_archive::restoreServiceArchive(
            archiveDirectory, *restoreRoot, progress);
    if (!result.ok) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(result.message), L'\n');
        return 1;
    }
    output(
        *restoreMode == ServiceRestoreMode::OriginalLocations
            ? L"Служба восстановлена в исходные места: "
            : L"Служба восстановлена под выбранный каталог: ",
        encoding::utf8_to_wstring(result.message), L'\n');
    bool deleteConfirmed = false;
    if (!assumeYes) {
        deleteConfirmed = confirm(
            L"Ручная проверка восстановленной службы завершена. "
            L"Удалить архивную копию службы?");
    }
    if (!searchengine_archive::shouldDeleteRestoredArchive(
            assumeYes, deleteConfirmed))
    {
        output(L"Архивная копия сохранена: ", archiveDirectory.wstring(), L'\n');
        return 0;
    }

    const auto deletion = searchengine_archive::deleteRestoredServiceArchive(
        archiveDirectory,
        [](const std::wstring& message) { output(message, L'\n'); });
    if (!deletion.ok) {
        outputError(
            L"ОШИБКА УДАЛЕНИЯ АРХИВА: ",
            encoding::utf8_to_wstring(deletion.message),
            L"\nСлужба уже работает из восстановленного места. "
            L"Архив сохранён либо удалён не полностью.\n");
        return 1;
    }
    output(L"Архивная копия службы удалена после повторной проверки.\n");
    return 0;
}

int runServiceCleanup(
    const fs::path& archiveDirectory,
    bool deleteMonthlyDatabases,
    bool assumeYes)
{
    if (!assumeYes && !confirm(
            L"Вы уже проверили поиск и работу службы из архивного каталога?"))
    {
        output(L"Очистка отменена. Исходное место сохранено.\n");
        return 2;
    }
    if (!assumeYes && !confirm(
            L"Удалить только исходные файлы, неизменность которых доказана manifest и SHA-256?"))
    {
        output(L"Очистка отменена.\n");
        return 2;
    }
    if (deleteMonthlyDatabases && !assumeYes && !confirm(
            L"Удалить также базы выбранного года, кроме декабрьской BASES_PRD?"))
    {
        deleteMonthlyDatabases = false;
    }
    const auto result = searchengine_archive::cleanupServiceArchiveSources(
        archiveDirectory,
        deleteMonthlyDatabases,
        [](const std::wstring& message) { output(message, L'\n'); });
    if (!result.ok) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(result.message), L'\n');
        return 1;
    }
    output(
        L"Исходное место очищено после полной предварительной проверки.\n",
        L"Декабрьская база BASES_PRD сохранена. Возврат через "
        L"--restore-service остаётся доступен.\n");
    return 0;
}

int interactiveServiceOperation()
{
    output(
        L"\nВыберите операцию со службой:\n",
        L"  1 - Перенести службу в архив и заморозить\n",
        L"  2 - Вернуть службу из архива и разморозить\n",
        L"  3 - После ручной проверки удалить старое место\n",
        L"  0 - Назад\n",
        L"Ваш выбор: ");
    const std::wstring choice = readLine();
    if (choice == L"0" || choice.empty())
        return 0;
    if (choice == L"2") {
        return runServiceRestore(
            promptDirectory(L"Каталог архивированной службы"),
            std::nullopt,
            std::nullopt,
            false);
    }
    if (choice == L"3") {
        return runServiceCleanup(
            promptDirectory(L"Каталог архивированной службы"), true, false);
    }
    if (choice != L"1")
        throw std::runtime_error("unknown service menu choice");

    const auto services = searchengine_archive::enumerateSearchEngineServices();
    if (services.empty())
        throw std::runtime_error("no eligible SearchEngine services were found");
    output(L"\nНайденные службы SearchEngine:\n");
    for (std::size_t index = 0; index < services.size(); ++index) {
        output(
            L"  ", (index + 1), L" - ",
            services[index].serviceName,
            L" [", services[index].dataDirectory.wstring(), L"]\n");
    }
    const auto selected = std::stoul(prompt(L"Номер службы", L"1"));
    if (selected == 0 || selected > services.size())
        throw std::runtime_error("service selection is outside the list");

    searchengine_archive::ServiceArchiveOptions options;
    options.serviceName = services[selected - 1].serviceName;
    options.archiveRoot = promptDirectory(L"Корневой каталог архива");
    return runServiceArchive(options, false);
}

void usage()
{
    output(
        L"SearchEngineArchive\n\n",
        L"  --year-only --year YYYY --archive-root DIR\n",
        L"      [--prm-monthly-dir DIR] [--prd-monthly-dir DIR] [--yes]\n",
        L"  --cleanup-year DIR [--delete-monthly-databases] [--yes]\n",
        L"  --archive-service --service-name NAME --archive-root DIR [--yes]\n",
        L"  --cleanup-service-source DIR [--keep-monthly-databases] [--yes]\n",
        L"  --restore-service DIR --restore-original [--yes]\n",
        L"  --restore-service DIR --restore-root DIR [--yes]\n");
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    try {
        std::vector<std::wstring> args(argv + 1, argv + argc);
        if (args.empty()) {
            output(
                L"Выберите операцию:\n",
                L"  1 - Перенести год без сервера\n",
                L"  2 - Архивировать или разморозить службу SearchEngine\n",
                L"  0 - Выход\n",
                L"Ваш выбор: ");
            const std::wstring choice = readLine();
            if (choice == L"0" || choice.empty())
                return 0;
            if (choice == L"1")
                return runYearOnly(interactiveYearOptions(), false);
            if (choice == L"2")
                return interactiveServiceOperation();
            throw std::runtime_error("unknown menu choice");
        }

        if (hasFlag(args, L"--help") || hasFlag(args, L"-h")) {
            usage();
            return 0;
        }
        if (hasFlag(args, L"--year-only")) {
            YearMoveOptions options;
            const auto year = option(args, L"--year");
            const auto root = option(args, L"--archive-root");
            if (!year || !root)
                throw std::runtime_error("--year and --archive-root are required");
            options.year = std::stoi(*year);
            options.archiveRoot = *root;
            options.prmMonthlyDirectory = option(args, L"--prm-monthly-dir").value_or(L"");
            options.prdMonthlyDirectory = option(args, L"--prd-monthly-dir").value_or(L"");
            return runYearOnly(options, hasFlag(args, L"--yes"));
        }
        if (const auto cleanup = option(args, L"--cleanup-year")) {
            return runCleanup(
                *cleanup,
                hasFlag(args, L"--delete-monthly-databases"),
                hasFlag(args, L"--yes"));
        }
        if (hasFlag(args, L"--archive-service")) {
            const auto serviceName = option(args, L"--service-name");
            const auto root = option(args, L"--archive-root");
            if (!serviceName || !root) {
                throw std::runtime_error(
                    "--service-name and --archive-root are required");
            }
            searchengine_archive::ServiceArchiveOptions options;
            options.serviceName = *serviceName;
            options.archiveRoot = *root;
            return runServiceArchive(options, hasFlag(args, L"--yes"));
        }
        if (const auto restore = option(args, L"--restore-service")) {
            const auto restoreRoot = option(args, L"--restore-root");
            const bool restoreOriginal = hasFlag(args, L"--restore-original");
            if (restoreOriginal && restoreRoot) {
                throw std::runtime_error(
                    "--restore-original and --restore-root cannot be used "
                    "together");
            }
            return runServiceRestore(
                *restore,
                restoreOriginal
                    ? std::optional<searchengine_archive::ServiceRestoreMode>(
                        searchengine_archive::ServiceRestoreMode::OriginalLocations)
                    : std::nullopt,
                restoreRoot ? std::optional<fs::path>(*restoreRoot) : std::nullopt,
                hasFlag(args, L"--yes"));
        }
        if (const auto cleanup = option(args, L"--cleanup-service-source")) {
            return runServiceCleanup(
                *cleanup,
                !hasFlag(args, L"--keep-monthly-databases"),
                hasFlag(args, L"--yes"));
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        outputError(
            L"ОШИБКА: ", encoding::utf8_to_wstring(error.what()), L'\n');
        return 1;
    }
}
