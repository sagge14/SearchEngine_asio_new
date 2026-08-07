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

Повторный запуск установщика предлагает безопасную переустановку и экспорт
старых настроек/логов. После успешного перехода службы в RUNNING старые
файлы удаляются из rollback.

Управление:

  Restart-BackupService.bat
  Uninstall-BackupService.bat
  sc query SearchEngineBackupService

Uninstall предлагает архив настроек и логов, после чего по подтверждению
удаляет службу, программу и ProgramData. Каталоги backup_dir / snapshots /
mirror history не удаляются.

Подробности: INSTALLATION_GUIDE_RU.txt
