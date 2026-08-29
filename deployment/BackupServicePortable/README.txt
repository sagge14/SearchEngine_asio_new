BackupService {{ARCHITECTURE}} — переносимый комплект
=====================================================

Архитектура: {{ARCHITECTURE}}
Минимальная система: {{MINIMUM_WINDOWS}}

На целевом компьютере PowerShell не требуется. Установка, переустановка,
перезапуск и полное удаление выполняются BAT-файлами. Проверка Backup.json
выполняется самим BackupService.exe (--validate-config).

Перед установкой:

1. Скопируйте ВСЮ папку комплекта на локальный диск целевого компьютера.
2. Откройте data\Backup.json и проверьте backup_dir и все src путей.
   По умолчанию — экономный профиль AutoPad (Backup.economical.json,
   strategy mirror_history). Создайте нужные локальные каталоги назначения.
3. Для сетевых ресурсов используйте UNC-пути, а не подключённые буквы дисков.
4. Запустите Install-BackupService.bat от имени администратора.

Рабочие файлы устанавливаются в Program Files\SearchEngineBackupService, а
конфигурация и логи — в C:\ProgramData\SearchEngineBackupService.
Каталоги снимков (backup_dir из JSON) живут отдельно и в комплект не входят.

Повторный запуск установщика предлагает безопасную переустановку: экспорт
настроек/логов, откат через rollback-каталоги, ожидание STOPPED и выхода PID.
После успешного перехода службы в RUNNING старые файлы удаляются из rollback.
Если после прерванного uninstall остались папки без службы, установщик
предложит их удалить или отменить установку.

Управление:

  Stop-BackupService.bat
  Start-BackupService.bat
  Restart-BackupService.bat
  Uninstall-BackupService.bat
  sc query SearchEngineBackupService

Stop и Start — штатная остановка и последующий запуск службы (новый процесс),
а не Windows Pause/Continue. После Stop → Start служба заново читает
установленный C:\ProgramData\SearchEngineBackupService\Backup.json
(для именованного экземпляра — его отдельный каталог в ProgramData).
Редактируйте именно этот установленный файл; изменение data\Backup.json
внутри исходного portable-комплекта не меняет уже установленную службу.
Остановленная служба с Automatic/Delayed Start снова запустится после
перезагрузки Windows; Stop предназначен для временной остановки. Для
долговременного отключения отдельно переведите Startup Type в Manual или
Disabled — эти скрипты Startup Type не меняют.

Uninstall пишет diagnostic log в %TEMP%, предлагает архив настроек и логов,
ждёт STOPPED/PID, удаляет файлы с retry, затем снимает регистрацию службы.
Каталоги backup_dir / snapshots / mirror history не удаляются.
После завершения нажмите 0, чтобы закрыть окно.

Подробности: INSTALLATION_GUIDE_RU.txt
