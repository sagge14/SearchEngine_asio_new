#include "ScriptMessages.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace script_messages {
namespace {

struct Message final {
    std::wstring_view id;
    std::wstring_view russian;
    std::wstring_view english;
};

constexpr Message kMessages[] = {
    {
        L"common.select",
        L"Ваш выбор: ",
        L"Select: "
    },
    {
        L"common.press_any_key",
        L"Нажмите любую клавишу для продолжения . . .",
        L"Press any key to continue . . ."
    },
    {
        L"common.press_zero",
        L"Нажмите 0, чтобы закрыть окно: ",
        L"Press 0 to close: "
    },
    {
        L"common.invalid_instance",
        L"ОШИБКА: Недопустимый идентификатор экземпляра \"{1}\".\n"
        L"Используйте от 1 до 32 латинских букв, цифр, знаков подчёркивания "
        L"или дефисов; первый символ должен быть буквой или цифрой.\n",
        L"ERROR: Invalid service instance id \"{1}\".\n"
        L"Use 1-32 ASCII letters, digits, underscores or hyphens; the first "
        L"character must be alphanumeric.\n"
    },
    {
        L"common.stop_timeout",
        L"Служба не остановилась за 120 секунд.\n"
        L"  1 - Принудительно завершить процесс службы и продолжить\n"
        L"  2 - Отмена без удаления файлов\n",
        L"The service did not stop within 120 seconds.\n"
        L"  1 - Force-terminate its process and continue\n"
        L"  2 - Cancel without deleting files\n"
    },
    {
        L"common.stopped_process",
        L"Служба ОСТАНОВЛЕНА, но её процесс с PID {1} всё ещё удерживает файлы.\n"
        L"  1 - Принудительно завершить этот процесс службы и продолжить\n"
        L"  2 - Отмена без удаления файлов\n",
        L"The service is STOPPED, but process PID {1} still holds files.\n"
        L"  1 - Force-terminate this service process and continue\n"
        L"  2 - Cancel without deleting files\n"
    },
    {
        L"common.directory_retry",
        L"\nКаталог всё ещё используется или доступ к нему запрещён:\n"
        L"  {1}\n"
        L"Закройте Проводник, Total Commander, программы для работы с базами "
        L"данных и другие приложения, которые могут держать этот каталог открытым.\n"
        L"  1 - Повторить удаление\n"
        L"  2 - Отмена с сохранением оставшихся файлов\n",
        L"\nDirectory is still in use or access is denied:\n"
        L"  {1}\n"
        L"Close Explorer, Total Commander, database tools and other programs "
        L"that may have this directory open.\n"
        L"  1 - Retry deletion\n"
        L"  2 - Cancel and preserve the remaining files\n"
    },
    {
        L"common.backup_destination",
        L"Диск или каталог назначения, например E:\\Backups: ",
        L"Destination disk or folder, for example E:\\Backups: "
    },
    {
        L"common.log_path",
        L"Журнал: {1}\n",
        L"Log: {1}\n"
    },
    {
        L"uninstall.confirm",
        L"\nБудут удалены служба Windows, правило брандмауэра, файлы программы,\n"
        L"настройки, индексы, базы данных, сообщения и журналы с этого компьютера.\n"
        L"  1 - Продолжить удаление\n"
        L"  2 - Отмена\n",
        L"\nThis will remove the Windows service, firewall rule, application files,\n"
        L"settings, indexes, databases, messages and logs from this computer.\n"
        L"  1 - Continue uninstall\n"
        L"  2 - Cancel\n"
    },
    {
        L"uninstall.not_admin",
        L"ОШИБКА: Запустите Uninstall-SearchEngineService.bat от имени администратора.\n",
        L"ERROR: Run Uninstall-SearchEngineService.bat as Administrator.\n"
    },
    {
        L"uninstall.stopping",
        L"Остановка службы {1}...\n",
        L"Stopping {1}...\n"
    },
    {
        L"uninstall.exporting",
        L"Экспорт предыдущих файлов...\n",
        L"Exporting previous files...\n"
    },
    {
        L"uninstall.delete_application",
        L"Удаление каталога программы...\n",
        L"Deleting application directory...\n"
    },
    {
        L"uninstall.delete_data",
        L"Удаление каталога данных...\n",
        L"Deleting data directory...\n"
    },
    {
        L"uninstall.delete_rollbacks",
        L"Удаление каталогов отката...\n",
        L"Deleting rollback directories...\n"
    },
    {
        L"uninstall.delete_service",
        L"Удаление регистрации службы...\n",
        L"Deleting service registration...\n"
    },
    {
        L"uninstall.success",
        L"\nСлужба {1} полностью удалена.\n",
        L"\n{1} was completely removed.\n"
    },
    {
        L"uninstall.backup_created",
        L"Перед удалением создана запрошенная резервная копия.\n",
        L"The requested backup was created before deletion.\n"
    },
    {
        L"uninstall.backup_menu",
        L"Резервная копия перед удалением:\n"
        L"  1 - Полная копия программы и данных (рекомендуется)\n"
        L"  2 - Только настройки и журналы\n"
        L"  3 - Не создавать резервную копию\n",
        L"Backup before uninstall:\n"
        L"  1 - Full application and data backup (recommended)\n"
        L"  2 - Settings and logs only\n"
        L"  3 - Do not create a backup\n"
    },
    {
        L"uninstall.no_backup",
        L"\nПРЕДУПРЕЖДЕНИЕ: после удаления резервной копии не будет.\n"
        L"  1 - Отмена (рекомендуется)\n"
        L"  2 - Продолжить без резервной копии\n",
        L"\nWARNING: no backup will be available after deletion.\n"
        L"  1 - Cancel (recommended)\n"
        L"  2 - Continue without backup\n"
    },
    {
        L"uninstall.service_retry",
        L"\nWindows всё ещё удерживает регистрацию службы:\n"
        L"  {1}\n"
        L"Закройте окно «Службы» (services.msc), Просмотр событий и другие "
        L"консоли управления, затем повторите попытку.\n"
        L"  1 - Повторить проверку регистрации\n"
        L"  2 - Отмена; служба останется остановленной\n",
        L"\nWindows still keeps the service registration open:\n"
        L"  {1}\n"
        L"Close the Services window (services.msc), Event Viewer and other "
        L"management consoles, then retry.\n"
        L"  1 - Retry the registration check\n"
        L"  2 - Cancel; the service remains stopped\n"
    },
    {
        L"uninstall.not_installed",
        L"Служба {1} и её файлы не установлены.\n",
        L"{1} and its files are not installed.\n"
    },
    {
        L"uninstall.no_services",
        L"Зарегистрированные службы SearchEngine не найдены. Ничего не удалено.\n",
        L"No registered SearchEngine services were found. Nothing was deleted.\n"
    },
    {
        L"uninstall.cancelled",
        L"Удаление отменено. Ничего не удалено.\n",
        L"Uninstall cancelled. Nothing was deleted.\n"
    },
    {
        L"uninstall.backup_failed",
        L"ОШИБКА: Не удалось создать резервную копию. Ничего не удалено.\n",
        L"ERROR: Backup failed. Nothing was deleted.\n"
    },
    {
        L"uninstall.app_delete_failed",
        L"ОШИБКА: Не удалось удалить каталог программы:\n  {1}\n",
        L"ERROR: Application directory could not be deleted:\n  {1}\n"
    },
    {
        L"uninstall.data_delete_failed",
        L"ОШИБКА: Не удалось удалить каталог данных:\n  {1}\n",
        L"ERROR: Data directory could not be deleted:\n  {1}\n"
    },
    {
        L"uninstall.app_rollback_delete_failed",
        L"ОШИБКА: Не удалось удалить один из каталогов отката программы.\n",
        L"ERROR: An application rollback directory could not be deleted.\n"
    },
    {
        L"uninstall.data_rollback_delete_failed",
        L"ОШИБКА: Не удалось удалить один из каталогов отката данных.\n",
        L"ERROR: A data rollback directory could not be deleted.\n"
    },
    {
        L"uninstall.service_delete_failed",
        L"ОШИБКА: Файлы удалены, но регистрация службы осталась.\n"
        L"Снова запустите этот деинсталлятор для службы {1}.\n",
        L"ERROR: Files were deleted, but the service registration remains.\n"
        L"Run this uninstaller again for {1}.\n"
    },
    {
        L"uninstall.failed",
        L"Удаление завершилось ошибкой. Неудалённые файлы сохранены.\n",
        L"Uninstall failed. Undeleted files were preserved.\n"
    },
    {
        L"uninstall.helper_failed",
        L"ОШИБКА: SearchEngineConfig не смог получить список установленных служб.\n",
        L"ERROR: SearchEngineConfig could not enumerate installed services.\n"
    },
    {
        L"configure.not_admin",
        L"ОШИБКА: Запустите Configure-SearchEngineService.bat от имени администратора.\n",
        L"ERROR: Run Configure-SearchEngineService.bat as Administrator.\n"
    },
    {
        L"configure.no_services",
        L"ОШИБКА: Установленные службы SearchEngine не найдены.\n",
        L"ERROR: No installed SearchEngine services were found.\n"
    },
    {
        L"configure.cancelled",
        L"Настройка отменена. Изменения не внесены.\n",
        L"Configuration cancelled. No changes were made.\n"
    },
    {
        L"configure.helper_failed",
        L"ОШИБКА: SearchEngineConfig не смог получить список установленных служб.\n",
        L"ERROR: SearchEngineConfig could not list installed services.\n"
    },
    {
        L"configure.service_missing",
        L"ОШИБКА: Служба {1} не установлена.\n",
        L"ERROR: Service {1} is not installed.\n"
    },
    {
        L"configure.resolving",
        L"Определение установленного каталога данных через диспетчер служб...\n",
        L"Resolving installed data directory from Service Control Manager...\n"
    },
    {
        L"configure.resolve_failed",
        L"ОШИБКА: Не удалось определить data-dir для службы {1}.\n"
        L"Проверьте, что в ImagePath службы присутствует аргумент --data-dir.\n",
        L"ERROR: Could not resolve data-dir for {1}.\n"
        L"Verify that the service has a --data-dir argument in its ImagePath.\n"
    },
    {
        L"configure.inspect_missing",
        L"ОШИБКА: inspect-installed не вернул data_dir.\n",
        L"ERROR: inspect-installed did not return data_dir.\n"
    },
    {
        L"configure.instance_info",
        L"Экземпляр : {1} ({2})\n"
        L"Данные    : {3}\n"
        L"Настройки : {4}\n"
        L"Подключение клиентов: {5}\n",
        L"Instance  : {1} ({2})\n"
        L"Data dir  : {3}\n"
        L"Settings  : {4}\n"
        L"Endpoint  : {5}\n"
    },
    {
        L"configure.settings_invalid",
        L"ОШИБКА: Текущий Settings.json отсутствует или содержит ошибки.\n",
        L"ERROR: Current Settings.json is missing or invalid.\n"
    },
    {
        L"configure.current",
        L"Текущий порт: {1}   год: {2}\n",
        L"Current port: {1}   year: {2}\n"
    },
    {
        L"configure.copy_failed",
        L"ОШИБКА: Не удалось скопировать Settings.json во временный файл.\n",
        L"ERROR: Cannot copy Settings.json to the temporary file.\n"
    },
    {
        L"configure.format_failed",
        L"ОШИБКА: Не удалось отформатировать Settings.json: временная копия "
        L"содержит недопустимый JSON.\n",
        L"ERROR: Cannot format Settings.json because the temporary copy "
        L"contains invalid JSON.\n"
    },
    {
        L"configure.validation_header",
        L"\n=== ПРОВЕРКА НЕ ПРОЙДЕНА ===\n",
        L"\n=== VALIDATION FAILED ===\n"
    },
    {
        L"configure.validation_footer",
        L"=============================\n\n"
        L"Изменённый Settings.json содержит ошибки.\n"
        L"  1 - Снова открыть редактор для исправления\n"
        L"  2 - Отменить настройку\n",
        L"=========================\n\n"
        L"The edited Settings.json is invalid.\n"
        L"  1 - Open the editor again to fix it\n"
        L"  2 - Cancel configuration\n"
    },
    {
        L"configure.old_new",
        L"\nСтарый порт: {1}   Новый порт: {2}\n"
        L"Старый год:  {3}   Новый год:  {4}\n",
        L"\nOld port: {1}   New port: {2}\n"
        L"Old year: {3}   New year: {4}\n"
    },
    {
        L"configure.endpoint_missing",
        L"ПРЕДУПРЕЖДЕНИЕ: client-endpoint.txt отсутствует и не будет обновлён.\n"
        L"После применения нового порта измените настройки подключения клиентов вручную.\n",
        L"WARNING: client-endpoint.txt does not exist; it will not be updated.\n"
        L"After applying the new port, update client connection settings manually.\n"
    },
    {
        L"configure.confirm_both",
        L"\nБудут обновлены: Settings.json и client-endpoint.txt\n",
        L"\nWill update: Settings.json and client-endpoint.txt\n"
    },
    {
        L"configure.confirm_settings",
        L"\nБудет обновлён только Settings.json\n",
        L"\nWill update: Settings.json only\n"
    },
    {
        L"configure.apply_menu",
        L"  1 - Применить: остановить службу, заменить файлы, запустить и проверить\n"
        L"  2 - Отмена\n",
        L"  1 - Apply: stop the service, replace files, start and verify\n"
        L"  2 - Cancel\n"
    },
    {
        L"configure.stopping",
        L"\nОстановка службы {1}...\n",
        L"\nStopping {1}...\n"
    },
    {
        L"configure.wait_stopped",
        L"Ожидание состояния ОСТАНОВЛЕНА... {1} с / {2} с\n",
        L"Still waiting for STOPPED... {1}s / {2}s\n"
    },
    {
        L"configure.stopped",
        L"Служба {1} ОСТАНОВЛЕНА.\n",
        L"{1} is STOPPED.\n"
    },
    {
        L"configure.applying",
        L"Применение конфигурации...\n",
        L"Applying configuration...\n"
    },
    {
        L"configure.apply_failed",
        L"ОШИБКА: settings-transaction-apply завершился ошибкой; возможно, "
        L"файлы уже были возвращены.\n"
        L"Проверьте вывод выше и файл {1}\\Settings.json вручную.\n",
        L"ERROR: settings-transaction-apply failed; files may be rolled back already.\n"
        L"Review the output above and check {1}\\Settings.json manually.\n"
    },
    {
        L"configure.starting",
        L"Запуск службы {1}...\n",
        L"Starting {1}...\n"
    },
    {
        L"configure.wait_running",
        L"Ожидание состояния РАБОТАЕТ... {1} с / {2} с\n",
        L"Waiting for RUNNING... {1}s / {2}s\n"
    },
    {
        L"configure.health",
        L"Служба {1} перешла в состояние РАБОТАЕТ; проверка PING/PONG на порту {2}...\n",
        L"{1} reached RUNNING; checking PING/PONG on port {2}...\n"
    },
    {
        L"configure.committing",
        L"PING/PONG подтверждён. Завершение транзакции...\n",
        L"PING/PONG confirmed. Committing transaction...\n"
    },
    {
        L"configure.commit_warning",
        L"ПРЕДУПРЕЖДЕНИЕ: settings-transaction-commit завершился ошибкой; "
        L"каталог отката мог сохраниться: {1}\n"
        L"Новая конфигурация активна, служба исправна. НЕ выполняйте откат.\n"
        L"Удалите каталог {1} вручную, когда это будет безопасно.\n",
        L"WARNING: settings-transaction-commit failed; rollback-dir may remain: {1}\n"
        L"The new configuration is active and the service is healthy. Do NOT roll back.\n"
        L"Remove {1} manually when safe.\n"
    },
    {
        L"configure.success",
        L"\n=== Конфигурация успешно применена ===\n"
        L"Экземпляр : {1} ({2})\n"
        L"Новый порт: {3}\n"
        L"Данные    : {4}\n"
        L"Журналы  : {4}\\logs\n",
        L"\n=== Configuration applied successfully ===\n"
        L"Instance : {1} ({2})\n"
        L"New port : {3}\n"
        L"Data dir : {4}\n"
        L"Logs     : {4}\\logs\n"
    },
    {
        L"configure.start_failed",
        L"ОШИБКА: После применения конфигурации служба не перешла в состояние РАБОТАЕТ.\n",
        L"ERROR: Service failed to reach RUNNING state after config apply.\n"
    },
    {
        L"configure.health_failed",
        L"ОШИБКА: Служба РАБОТАЕТ, но проверка PING/PONG на порту {1} не пройдена.\n"
        L"Запускается откат...\n",
        L"ERROR: Service is RUNNING but PING/PONG failed on port {1}.\n"
        L"Initiating rollback...\n"
    },
    {
        L"configure.unexpected_state",
        L"ОШИБКА: Служба находится в неожиданном состоянии; безопасный откат невозможен.\n",
        L"ERROR: Service is in an unexpected state; cannot safely perform rollback.\n"
    },
    {
        L"configure.stopping_rollback",
        L"Остановка службы {1} перед откатом...\n",
        L"Stopping {1} before rollback...\n"
    },
    {
        L"configure.wait_rollback",
        L"Ожидание состояния ОСТАНОВЛЕНА для отката... {1} с / {2} с\n",
        L"Waiting for STOPPED (rollback)... {1}s / {2}s\n"
    },
    {
        L"configure.rollback_cannot_stop",
        L"ОШИБКА: Служба не перешла в состояние ОСТАНОВЛЕНА за {1} секунд.\n"
        L"Откат прерван для сохранения целостности данных. Файлы не восстановлены.\n"
        L"  Служба        : {2}\n"
        L"  Каталог данных: {3}\n"
        L"  Каталог отката: {4}\n"
        L"  Старый порт   : {5}\n"
        L"  Новый порт    : {6}\n"
        L"Снимки сохранены в каталоге отката для ручного восстановления.\n",
        L"ERROR: Service did not reach STOPPED within {1} seconds.\n"
        L"Rollback aborted to preserve data integrity. No files were restored.\n"
        L"  Service    : {2}\n"
        L"  Data-dir   : {3}\n"
        L"  Rollback   : {4}\n"
        L"  Old port   : {5}\n"
        L"  New port   : {6}\n"
        L"Snapshots preserved in rollback-dir for manual recovery.\n"
    },
    {
        L"configure.restoring_files",
        L"Служба ОСТАНОВЛЕНА. Восстановление файлов...\n",
        L"Service is STOPPED. Restoring files...\n"
    },
    {
        L"configure.rollback_files_failed",
        L"ОШИБКА: settings-transaction-rollback завершился ошибкой. Снимки сохранены.\n"
        L"  Каталог отката: {1}\n"
        L"НЕ завершайте транзакцию: старая конфигурация могла восстановиться "
        L"частично. Выполните ручное восстановление.\n",
        L"ERROR: settings-transaction-rollback failed. Snapshots preserved.\n"
        L"  Rollback-dir: {1}\n"
        L"Do NOT commit; old configuration may be partially restored. Recover manually.\n"
    },
    {
        L"configure.files_restored",
        L"Файлы старой конфигурации восстановлены.\n",
        L"Files restored to the old configuration.\n"
    },
    {
        L"configure.firewall_restore_failed",
        L"ОШИБКА: Не удалось восстановить созданное установщиком правило брандмауэра.\n"
        L"Файлы старой конфигурации восстановлены, но правило брандмауэра может "
        L"по-прежнему указывать новый порт {1}.\n"
        L"Снимки сохранены; НЕ завершайте транзакцию. Восстановите брандмауэр вручную.\n"
        L"  Каталог отката: {2}\n",
        L"ERROR: Firewall restore failed for the installer-owned rule.\n"
        L"The old service configuration has been restored in files, but the firewall "
        L"rule may still point to the new port {1}.\n"
        L"Snapshots preserved; do NOT commit. Restore the firewall manually.\n"
        L"  Rollback-dir: {2}\n"
    },
    {
        L"configure.restart_old",
        L"Перезапуск службы со старой конфигурацией (порт {1})...\n",
        L"Restarting the service on the old configuration (port {1})...\n"
    },
    {
        L"configure.old_start_failed",
        L"ОШИБКА: После отката старая служба не перешла в состояние РАБОТАЕТ.\n"
        L"Снимки сохранены для ручного восстановления: {1}\n"
        L"Подробности находятся в {2}\\logs.\n",
        L"ERROR: Old service did not reach RUNNING after rollback.\n"
        L"Snapshots preserved for manual recovery: {1}\n"
        L"Check {2}\\logs for details.\n"
    },
    {
        L"configure.old_health_failed",
        L"ОШИБКА: Старая служба РАБОТАЕТ, но проверка PING/PONG на порту {1} не пройдена.\n"
        L"Снимки сохранены для ручного восстановления: {2}\n"
        L"Подробности находятся в {3}\\logs.\n",
        L"ERROR: Old service is RUNNING but PING/PONG on port {1} failed.\n"
        L"Snapshots preserved for manual recovery: {2}\n"
        L"Check {3}\\logs for details.\n"
    },
    {
        L"configure.rollback_success",
        L"Откат успешно выполнен. Служба РАБОТАЕТ на старом порту {1}.\n",
        L"Rollback successful. Service is RUNNING on old port {1}.\n"
    },
    {
        L"configure.rollback_commit_warning",
        L"ПРЕДУПРЕЖДЕНИЕ: settings-transaction-commit завершился ошибкой; "
        L"каталог отката сохранён: {1}\n"
        L"Удалите его вручную, когда это будет безопасно.\n",
        L"WARNING: settings-transaction-commit failed; rollback-dir remains: {1}\n"
        L"Remove it manually when safe.\n"
    },
    {
        L"configure.stop_failed",
        L"ОШИБКА: Служба не перешла в состояние ОСТАНОВЛЕНА за {1} секунд.\n"
        L"Изменения конфигурации не внесены.\n",
        L"ERROR: Service did not reach STOPPED state within {1} seconds.\n"
        L"No configuration changes were made.\n"
    },
    {
        L"configure.endpoint_temp_failed",
        L"ОШИБКА: Не удалось создать временный файл подключения клиентов.\n",
        L"ERROR: Failed to build the endpoint temporary file.\n"
    },
    {
        L"configure.firewall_none",
        L"ПРИМЕЧАНИЕ: Созданное установщиком правило брандмауэра не найдено; "
        L"обновление брандмауэра пропущено.\n",
        L"NOTE: No installer-owned firewall rule was found; skipping the firewall update.\n"
    },
    {
        L"configure.firewall_update",
        L"Обновление правила брандмауэра \"{1}\": порт {2} -> {3}\n",
        L"Updating firewall rule \"{1}\": port {2} -> {3}\n"
    },
    {
        L"configure.firewall_update_failed",
        L"ОШИБКА: Не удалось обновить переносимое правило брандмауэра \"{1}\".\n",
        L"ERROR: Could not update portable firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_rename",
        L"Переименование правила брандмауэра: \"{1}\" -> \"{2}\"\n",
        L"Renaming firewall rule: \"{1}\" -> \"{2}\"\n"
    },
    {
        L"configure.firewall_program_empty",
        L"ОШИБКА: PROGRAM_PATH пуст; безопасно пересоздать правило брандмауэра "
        L"PowerShell невозможно.\n",
        L"ERROR: PROGRAM_PATH is empty; cannot safely recreate the PowerShell-owned "
        L"firewall rule.\n"
    },
    {
        L"configure.firewall_delete_old_failed",
        L"ОШИБКА: Не удалось удалить старое правило брандмауэра PowerShell \"{1}\".\n",
        L"ERROR: Could not delete old PowerShell firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_create_new_failed",
        L"ОШИБКА: Не удалось создать новое правило брандмауэра PowerShell \"{1}\".\n",
        L"ERROR: Could not create new PowerShell firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_restore",
        L"Восстановление правила брандмауэра \"{1}\": порт {2} -> {3}\n",
        L"Restoring firewall rule \"{1}\": port {2} -> {3}\n"
    },
    {
        L"configure.firewall_restore_failed_rule",
        L"ОШИБКА: Не удалось восстановить переносимое правило брандмауэра \"{1}\".\n",
        L"ERROR: Could not restore portable firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_restore_rename",
        L"Восстановление правила брандмауэра: \"{1}\" -> \"{2}\"\n",
        L"Restoring firewall rule: \"{1}\" -> \"{2}\"\n"
    },
    {
        L"configure.firewall_program_restore_empty",
        L"ОШИБКА: PROGRAM_PATH пуст; безопасно восстановить правило брандмауэра "
        L"PowerShell невозможно.\n",
        L"ERROR: PROGRAM_PATH is empty; cannot safely restore the PowerShell-owned "
        L"firewall rule.\n"
    },
    {
        L"configure.firewall_delete_new_failed",
        L"ОШИБКА: Не удалось удалить НОВОЕ правило брандмауэра PowerShell \"{1}\".\n",
        L"ERROR: Could not delete the NEW PowerShell firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_restore_old_failed",
        L"ОШИБКА: Не удалось восстановить правило брандмауэра PowerShell \"{1}\".\n",
        L"ERROR: Could not restore PowerShell firewall rule \"{1}\".\n"
    },
    {
        L"configure.firewall_verify_old_missing",
        L"ОШИБКА: Проверка восстановления брандмауэра не пройдена: старое правило "
        L"\"{1}\" не найдено.\n",
        L"ERROR: Firewall restore verification failed: OLD rule \"{1}\" was not found.\n"
    },
    {
        L"configure.firewall_verify_new_exists",
        L"ОШИБКА: Проверка восстановления брандмауэра не пройдена: новое правило "
        L"\"{1}\" всё ещё существует.\n",
        L"ERROR: Firewall restore verification failed: NEW rule \"{1}\" still exists.\n"
    },
    {
        L"install.header",
        L"Установщик переносимого SearchEngineService ({1})\n"
        L"Экземпляр: {2} ({3})\n\n",
        L"SearchEngineService portable installer ({1})\n"
        L"Instance: {2} ({3})\n\n"
    },
    {
        L"install.local_argument_conflict",
        L"ОШИБКА: /LocalMachine нельзя совмещать с /validate или явным "
        L"идентификатором экземпляра.\n",
        L"ERROR: /LocalMachine cannot be combined with /validate or an "
        L"explicit instance id.\n"
    },
    {
        L"install.local_existing",
        L"ОШИБКА: Годовая служба {1} уже существует. Автоматическая локальная "
        L"установка не заменяет существующую службу.\n",
        L"ERROR: Year service {1} already exists. Local-machine setup does "
        L"not replace an existing service.\n"
    },
    {
        L"install.local_leftovers",
        L"ОШИБКА: Обнаружены оставшиеся файлы годового экземпляра. "
        L"Автоматическое удаление запрещено.\n  Программа: {1}\n  Данные: {2}\n",
        L"ERROR: Leftover year-instance files were found. Automatic deletion "
        L"is disabled.\n  Application: {1}\n  Data: {2}\n"
    },
    {
        L"install.existing",
        L"Найдена установленная служба SearchEngineService.\n"
        L"  1 - Переустановить или обновить (рекомендуется)\n"
        L"  2 - Отмена\n",
        L"An installed SearchEngineService was found.\n"
        L"  1 - Reinstall or update it (recommended)\n"
        L"  2 - Cancel\n"
    },
    {
        L"install.leftovers",
        L"\nПосле предыдущего незавершённого удаления остались файлы,\n"
        L"но служба Windows {1} не зарегистрирована.\n",
        L"\nFiles remain from an earlier incomplete uninstall, but the Windows\n"
        L"service {1} is not registered.\n"
    },
    {
        L"install.application_path",
        L"  Программа: {1}\n",
        L"  Application: {1}\n"
    },
    {
        L"install.data_path",
        L"  Данные:    {1}\n",
        L"  Data:        {1}\n"
    },
    {
        L"install.leftovers_warning",
        L"\nУдаление этих каталогов безвозвратно удалит их настройки, индексы,\n"
        L"базы данных, сообщения и журналы.\n"
        L"  1 - Удалить оставшиеся каталоги и продолжить установку\n"
        L"  2 - Отмена (рекомендуется, если файлы нужно сохранить)\n",
        L"\nRemoving these folders permanently deletes their settings, indexes,\n"
        L"databases, messages and logs.\n"
        L"  1 - Delete the leftover folders and continue installation\n"
        L"  2 - Cancel (recommended if the files must be preserved)\n"
    },
    {
        L"install.config_header",
        L"\nВыбранная конфигурация:\n",
        L"\nSelected configuration:\n"
    },
    { L"install.config_port", L"  Порт:                 {1}\n", L"  Port:                 {1}\n" },
    { L"install.config_year", L"  Год:                  {1}\n", L"  Year:                 {1}\n" },
    { L"install.config_threads", L"  Исполнительные потоки:{1}\n", L"  Executor threads:     {1}\n" },
    { L"install.config_timeout", L"  Тайм-аут одного файла: {1} с\n", L"  One-file timeout:     {1} sec\n" },
    { L"install.config_prm_on", L"  Короткое содержимое PRM: включено\n", L"  PRM short content:    enabled\n" },
    { L"install.config_prm_off", L"  Короткое содержимое PRM: отключено\n", L"  PRM short content:    disabled\n" },
    { L"install.config_catalog", L"  Каталог документов:   {1}\n\n", L"  Document catalog:     {1}\n\n" },
    {
        L"install.step_runtime",
        L"[1/8] Проверка Microsoft Visual C++ Runtime...\n",
        L"[1/8] Ensuring Microsoft Visual C++ Runtime...\n"
    },
    {
        L"install.runtime_missing",
        L"Файлы Visual C++ Runtime для архитектуры {1} не найдены.\n"
        L"  1 - Установить или обновить комплектный пакет (рекомендуется)\n"
        L"  2 - Пропустить установку Runtime\n",
        L"Visual C++ Runtime files were not detected for architecture {1}.\n"
        L"  1 - Install or update the packaged redistributable (recommended)\n"
        L"  2 - Skip redistributable setup\n"
    },
    {
        L"install.runtime_present",
        L"Файлы Visual C++ Runtime найдены на этом компьютере.\n"
        L"  1 - Пропустить установку Runtime (рекомендуется)\n"
        L"  2 - Всё равно установить или обновить комплектный пакет\n",
        L"Visual C++ Runtime files were found on this computer.\n"
        L"  1 - Skip redistributable setup (recommended)\n"
        L"  2 - Install or update the packaged redistributable anyway\n"
    },
    {
        L"install.runtime_installing",
        L"Установка Microsoft Visual C++ Runtime...\n",
        L"Installing Microsoft Visual C++ Runtime...\n"
    },
    {
        L"install.runtime_failed",
        L"ПРЕДУПРЕЖДЕНИЕ: Установка Visual C++ Runtime завершилась с кодом {1}.\n"
        L"SearchEngineConfig.exe уже запускался, поэтому Runtime, вероятно, установлен.\n"
        L"  1 - Продолжить установку\n"
        L"  2 - Отмена\n",
        L"WARNING: Visual C++ Runtime setup failed with exit code {1}.\n"
        L"SearchEngineConfig.exe already ran, so the runtime is likely already present.\n"
        L"  1 - Continue installation\n"
        L"  2 - Cancel\n"
    },
    {
        L"install.runtime_skip_flag",
        L"Установка Visual C++ Runtime пропущена из-за параметра /SkipVcRedist.\n",
        L"Skipping Visual C++ Runtime setup because /SkipVcRedist was specified.\n"
    },
    {
        L"install.runtime_skip_choice",
        L"Установка Visual C++ Runtime пропущена по выбору пользователя.\n",
        L"Skipping Visual C++ Runtime redistributable setup by user choice.\n"
    },
    {
        L"install.runtime_restart",
        L"ПРЕДУПРЕЖДЕНИЕ: После установки необходимо перезапустить Windows.\n",
        L"WARNING: Windows must be restarted after the installation.\n"
    },
    { L"install.step_stop", L"[2/8] Остановка установленной службы...\n", L"[2/8] Stopping the installed service...\n" },
    { L"install.step_export", L"[3/8] Экспорт предыдущей установки...\n", L"[3/8] Exporting the previous installation...\n" },
    { L"install.preparing_rollback", L"Подготовка отката программы...\n", L"Preparing application rollback...\n" },
    { L"install.step_copy", L"[4/8] Копирование файлов программы...\n", L"[4/8] Copying application files...\n" },
    {
        L"install.attachments_menu",
        L"\nИспользуется ли на этом экземпляре GET_ATTACHMENTS / «Сохранить вложения»?\n"
        L"  1 - Да\n"
        L"  2 - Нет\n",
        L"\nIs GET_ATTACHMENTS / \"Save attachments\" used on this server instance?\n"
        L"  1 - Yes\n"
        L"  2 - No\n"
    },
    { L"install.step_fresh_data", L"[5/8] Создание каталога данных из пакета...\n", L"[5/8] Creating data directory from package...\n" },
    { L"install.step_update_data", L"[5/8] Обновление управляемых файлов среды выполнения...\n", L"[5/8] Updating managed runtime files...\n" },
    { L"install.prefix_package_missing", L"В пакете отсутствует data\\prefix_map.json.\n", L"The package does not contain data\\prefix_map.json.\n" },
    { L"install.prefix_package_invalid", L"Файл data\\prefix_map.json в пакете содержит ошибки.\n", L"Package data\\prefix_map.json is invalid.\n" },
    { L"install.prefix_instance_missing", L"У этого экземпляра отсутствует prefix_map.json.\n", L"This instance has no prefix_map.json.\n" },
    { L"install.prefix_instance_invalid", L"Существующий prefix_map.json содержит ошибки и не был заменён.\n", L"Existing prefix_map.json is invalid and was not replaced.\n" },
    {
        L"install.prefix_warning",
        L"\nПРЕДУПРЕЖДЕНИЕ: GET_ATTACHMENTS не будет работать, пока не появится "
        L"исправный файл:\n  {1}\n",
        L"\nWARNING: GET_ATTACHMENTS will not work until a valid file exists:\n  {1}\n"
    },
    {
        L"install.continue_cancel",
        L"  1 - Продолжить\n  2 - Отмена\n",
        L"  1 - Continue\n  2 - Cancel\n"
    },
    { L"install.step_register", L"[6/8] Регистрация и настройка службы Windows...\n", L"[6/8] Registering and configuring the Windows service...\n" },
    {
        L"install.localsystem",
        L"Учётная запись службы: LocalSystem\n"
        L"Каталоги среды выполнения должны быть доступны LocalSystem.\n"
        L"Подключённые диски пользователя недоступны службе Windows.\n",
        L"Service account: LocalSystem\n"
        L"Runtime paths must be accessible to LocalSystem.\n"
        L"User mapped drives are not available to the Windows service.\n"
    },
    { L"install.step_firewall", L"[7/8] Настройка брандмауэра Windows для TCP-порта {1}...\n", L"[7/8] Configuring Windows Firewall for TCP port {1}...\n" },
    {
        L"install.local_auth",
        L"Создание computer-токена local-machine/operator и регистрация "
        L"в базе авторизации...\n",
        L"Creating the local-machine/operator computer token and registering "
        L"it in the authorization database...\n"
    },
    {
        L"install.local_auth_failed",
        L"ОШИБКА: Не удалось создать или зарегистрировать локальный "
        L"computer-токен.\n",
        L"ERROR: The local computer token could not be created or registered.\n"
    },
    {
        L"install.local_initial_update",
        L"Одноразовое первоначальное сканирование и построение SQLite-индекса...\n",
        L"Running the one-time initial scan and SQLite index build...\n"
    },
    { L"install.step_start", L"[8/8] Запуск службы и проверка PING/PONG...\n", L"[8/8] Starting the service and checking PING/PONG...\n" },
    { L"install.old_app_warning", L"ПРЕДУПРЕЖДЕНИЕ: Не удалось удалить старый каталог программы: {1}\n", L"WARNING: Old application directory could not be removed: {1}\n" },
    { L"install.runtime_tx_warning", L"ПРЕДУПРЕЖДЕНИЕ: Каталог транзакции оставлен для диагностики: {1}\n", L"WARNING: Runtime transaction directory was left for diagnostics: {1}\n" },
    {
        L"install.success",
        L"\nУстановка успешно завершена.\n"
        L"Служба:     {1} (РАБОТАЕТ, PING/PONG подтверждён)\n"
        L"Экземпляр:  {4}\n"
        L"Адрес:      {6}:{5}\n"
        L"TCP-порт:   {5} (правило брандмауэра: {1} TCP)\n"
        L"Учётная запись службы: LocalSystem\n"
        L"Каталоги среды выполнения должны быть доступны LocalSystem.\n"
        L"Подключённые диски пользователя недоступны службе Windows.\n"
        L"Установка:  {2}\n"
        L"Программа:  {2}\\bin\\SearchEngine.exe\n"
        L"Данные:     {3}\n"
        L"Журналы:   {3}\\logs\n"
        L"Подсказка для клиента: {3}\\client-endpoint.txt\n\n",
        L"\nInstallation completed successfully.\n"
        L"Service:     {1} (RUNNING and PING/PONG OK)\n"
        L"Instance:    {4}\n"
        L"Endpoint:    {6}:{5}\n"
        L"TCP port:    {5} (firewall rule: {1} TCP)\n"
        L"Service account: LocalSystem\n"
        L"Runtime paths must be accessible to LocalSystem.\n"
        L"User mapped drives are not available to the Windows service.\n"
        L"Install root: {2}\n"
        L"Application: {2}\\bin\\SearchEngine.exe\n"
        L"Data:        {3}\n"
        L"Logs:        {3}\\logs\n"
        L"Client hint: {3}\\client-endpoint.txt\n\n"
    },
    {
        L"install.local_success",
        L"Локальный computer-токен зарегистрирован и сохранён:\n  {1}\n"
        L"Первоначальная индексация: ВЫПОЛНЕНА УСПЕШНО (одноразовый запуск).\n"
        L"Автосканирование при запуске: ОТКЛЮЧЕНО (scan_on_startup=false).\n",
        L"The local computer token was registered and saved:\n  {1}\n"
        L"Initial indexing: COMPLETED SUCCESSFULLY (one-time run).\n"
        L"Startup scan: DISABLED (scan_on_startup=false).\n"
    },
    {
        L"install.local_initial_update_failed",
        L"ПРЕДУПРЕЖДЕНИЕ: Служба установлена и работает, но первоначальная "
        L"индексация завершилась с кодом {1}.\n"
        L"Повтор: \"{3}\\SearchEngine.exe\" --initial-update --data-dir \"{2}\"\n",
        L"WARNING: The service is installed and running, but initial indexing "
        L"failed with exit code {1}.\n"
        L"Retry: \"{3}\\SearchEngine.exe\" --initial-update --data-dir \"{2}\"\n"
    },
    { L"install.validated", L"Проверка пакета успешно завершена.\n", L"Package verification completed successfully.\n" },
    {
        L"install.backup_menu",
        L"\nРезервная копия перед заменой установленных файлов:\n"
        L"  1 - Полная копия программы и данных (рекомендуется)\n"
        L"  2 - Только настройки и журналы\n"
        L"  3 - Не создавать резервную копию\n",
        L"\nBackup before replacing the installed files:\n"
        L"  1 - Full application and data backup (recommended)\n"
        L"  2 - Settings and logs only\n"
        L"  3 - Do not create a backup\n"
    },
    {
        L"install.no_export",
        L"Пропуск дополнительного экспорта не удаляет ProgramData.\n"
        L"Индекс, база авторизации, сообщения, журналы, prefix_map.json,\n"
        L"пользовательский ignore.txt и другие файлы среды выполнения остаются на месте.\n"
        L"Экспорт является только дополнительной резервной копией оператора.\n"
        L"  1 - Отмена (рекомендуется)\n"
        L"  2 - Продолжить без экспорта\n",
        L"Skipping the optional export does not delete ProgramData.\n"
        L"The index, authorization database, messages, logs, prefix_map.json,\n"
        L"user ignore.txt and other runtime files stay in place. Export is an\n"
        L"extra operator backup only.\n"
        L"  1 - Cancel (recommended)\n"
        L"  2 - Continue without export\n"
    },
    { L"install.rollback_prepare_failed", L"ОШИБКА: Не удалось переместить предыдущую программу в каталог отката.\n", L"ERROR: Cannot move the previous application into a rollback directory.\n" },
    { L"install.port_in_use", L"ОШИБКА: TCP-порт {1} уже занят другим процессом.\n", L"ERROR: TCP port {1} is already occupied by another process.\n" },
    { L"install.copy_failed", L"ОШИБКА: Не удалось скопировать файлы установки.\n", L"ERROR: Cannot copy installation files.\n" },
    { L"install.service_setup_failed", L"ОШИБКА: Не удалось настроить службу Windows.\n", L"ERROR: Cannot configure the Windows service.\n" },
    { L"install.service_start_failed", L"ОШИБКА: Служба не перешла в состояние РАБОТАЕТ за 120 секунд.\n", L"ERROR: The service did not reach RUNNING state within 120 seconds.\n" },
    { L"install.health_failed", L"ОШИБКА: Процесс службы работает, но не ответил на PING за отведённое время.\nПроверьте журналы в {1}\\logs.\n", L"ERROR: The service process is running but did not answer PING within the timeout.\nCheck logs in {1}\\logs.\n" },
    { L"install.restoring", L"Восстановление предыдущей рабочей установки...\n", L"Restoring the previous working installation...\n" },
    { L"install.rollback_ok", L"Предыдущая программа, управляемые файлы и PING/PONG на старом порту восстановлены.\n", L"Previous application, managed files and old-port PING/PONG were restored.\n" },
    { L"install.rollback_no_health", L"Предыдущая программа и управляемые файлы восстановлены.\nСтарый порт службы неизвестен, поэтому PING/PONG не проверялся.\n", L"Previous application and managed files were restored.\nOld service port is unknown, so PING/PONG was not verified.\n" },
    { L"install.runtime_transaction", L"  Транзакция среды выполнения: {1}\n", L"  Runtime transaction: {1}\n" },
    { L"install.application_rollback", L"  Откат программы: {1}\n", L"  Application rollback: {1}\n" },
    { L"install.rollback_stop_failed", L"ОШИБКА: Не удалось остановить новую службу для автоматического отката.\n", L"ERROR: The new service could not be stopped for automatic rollback.\n" },
    { L"install.rollback_files_failed", L"ОШИБКА: Автоматический откат не смог восстановить предыдущий каталог программы.\n", L"ERROR: Automatic rollback could not restore the previous application directory.\n" },
    { L"install.rollback_runtime_failed", L"ОШИБКА: Откат управляемых файлов среды выполнения не завершён.\nКаталог транзакции сохранён:\n  {1}\n", L"ERROR: Runtime managed-file rollback did not complete.\nTransaction directory preserved:\n  {1}\n" },
    { L"install.rollback_health_failed", L"ОШИБКА: Предыдущая служба запущена, но PING/PONG на старом порту не подтверждён.\n", L"ERROR: The previous service was started but old-port PING/PONG was not confirmed.\n" },
    { L"install.rollback_incomplete", L"ОШИБКА: Автоматический откат не полностью восстановил предыдущую установку.\n", L"ERROR: Automatic rollback did not fully restore the previous installation.\n" },
    { L"install.unknown_argument", L"ОШИБКА: Неизвестный аргумент \"{1}\".\nПоддерживаются: /validate, /SkipVcRedist и необязательный идентификатор экземпляра.\n", L"ERROR: Unknown argument \"{1}\".\nSupported: /validate, /SkipVcRedist, and an optional instance id.\n" },
    { L"install.not_admin", L"ОШИБКА: Запустите Install-SearchEngineService.bat от имени администратора.\n", L"ERROR: Run Install-SearchEngineService.bat as Administrator.\n" },
    { L"install.package_missing", L"ОШИБКА: Переносимый пакет неполон. Скопируйте всю папку заново.\n", L"ERROR: The portable package is incomplete. Copy the entire folder again.\n" },
    { L"install.package_damaged", L"ОШИБКА: Проверка пакета не пройдена. Скопируйте всю папку заново.\n", L"ERROR: Package verification failed. Copy the entire folder again.\n" },
    { L"install.helper_failed", L"ОШИБКА: SearchEngineConfig не смог проверить или создать настройки.\n", L"ERROR: SearchEngineConfig could not validate or generate settings.\n" },
    { L"install.leftover_app_failed", L"ОШИБКА: Не удалось удалить оставшийся каталог программы: {1}\n", L"ERROR: Leftover application directory could not be deleted: {1}\n" },
    { L"install.leftover_data_failed", L"ОШИБКА: Не удалось удалить оставшийся каталог данных: {1}\n", L"ERROR: Leftover data directory could not be deleted: {1}\n" },
    { L"install.partial_files", L"Частичные файлы сохранены для диагностики:\n  {1}\n  {2}\n", L"Partial files were preserved for diagnostics:\n  {1}\n  {2}\n" },
    { L"install.failed", L"\nУстановка завершилась ошибкой. Прочитайте сообщение выше.\n", L"\nInstallation failed. Read the error above.\n" },
    { L"install.cancelled", L"Установка отменена. Установленные файлы не изменены.\n", L"Installation cancelled. No installed files were changed.\n" }
};

void replaceAll(
    std::wstring& value,
    const std::wstring& token,
    const std::wstring& replacement)
{
    std::size_t offset = 0;
    while ((offset = value.find(token, offset)) != std::wstring::npos) {
        value.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

} // namespace

std::wstring render(
    std::wstring_view id,
    bool russian,
    const std::vector<std::wstring>& arguments)
{
    const auto found = std::find_if(
        std::begin(kMessages),
        std::end(kMessages),
        [&](const Message& message) { return message.id == id; });
    if (found == std::end(kMessages)) {
        throw std::runtime_error("unknown script message id");
    }

    std::wstring result(russian ? found->russian : found->english);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        replaceAll(
            result,
            L"{" + std::to_wstring(index + 1) + L"}",
            arguments[index]);
    }
    return result;
}

void validateCatalog()
{
    for (std::size_t index = 0; index < std::size(kMessages); ++index) {
        const Message& message = kMessages[index];
        std::string id;
        id.reserve(message.id.size());
        for (wchar_t character : message.id) {
            id.push_back(static_cast<char>(character));
        }
        if (message.id.empty() || message.russian.empty() || message.english.empty()) {
            throw std::runtime_error("empty script message field: " + id);
        }
        for (wchar_t character : message.id) {
            const bool valid =
                (character >= L'a' && character <= L'z') ||
                (character >= L'0' && character <= L'9') ||
                character == L'.' || character == L'_' || character == L'-';
            if (!valid) {
                throw std::runtime_error("invalid script message id: " + id);
            }
        }
        for (wchar_t character : message.english) {
            if ((character >= L'А' && character <= L'я') ||
                character == L'Ё' || character == L'ё')
            {
                throw std::runtime_error(
                    "English script message contains Cyrillic text: " + id);
            }
        }
        for (int placeholder = 1; placeholder <= 8; ++placeholder) {
            const std::wstring token =
                L"{" + std::to_wstring(placeholder) + L"}";
            const bool russianHas = message.russian.find(token) != std::wstring::npos;
            const bool englishHas = message.english.find(token) != std::wstring::npos;
            if (russianHas != englishHas) {
                throw std::runtime_error(
                    "script message placeholder mismatch: " + id);
            }
        }
        for (std::size_t other = index + 1;
             other < std::size(kMessages);
             ++other)
        {
            if (message.id == kMessages[other].id) {
                throw std::runtime_error("duplicate script message id: " + id);
            }
        }
    }
}

} // namespace script_messages
