# BackupService как Windows-служба

`BackupService.exe` использует один и тот же `BackupServiceApplication` в
консоли и под Service Control Manager (SCM). Снимки, `mirror_history`,
SQLite Online Backup и формат `Backup.json` от режима запуска не зависят.

## Режимы запуска и пути

```powershell
BackupService.exe --console --config C:\Backup\Backup.all.json --data-dir C:\Backup
BackupService.exe --once --config C:\Backup\Backup.all.json --data-dir C:\Backup
BackupService.exe --service --service-name SearchEngineBackupService `
  --config C:\Backup\Backup.all.json --data-dir C:\Backup
```

- без режима или с `--console` запускается периодический console mode;
- `--once` выполняет каждую группу один раз;
- `--service` подключается к SCM и не использует stdin или console signals.

По умолчанию `data-dir` равен каталогу `BackupService.exe`, конфигурация —
`<data-dir>\Backup.json`, логи — `<data-dir>\logs`. Относительный
`--data-dir` разрешается от каталога exe, относительный `--config` — от
`data-dir`. Относительные `src` и `backup_dir` внутри JSON разрешаются от
каталога конфигурации. Поэтому SCM не создаёт рабочие файлы в
`C:\Windows\System32`.

## Сборка

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --target BackupService

cmake --preset windows-x86
cmake --build --preset windows-x86-release --target BackupService
```

Используйте Release-бинарник той разрядности, которая нужна на машине.
Служба остаётся console-subsystem executable; нестандартной точки входа нет.

## Установка одного экземпляра

Из PowerShell с правами администратора:

```powershell
tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath F:\SearchEngine_asio_new\out\build\windows-x64\Release\BackupService.exe `
  -ConfigPath C:\Backup\Backup.all.json `
  -DataDir C:\Backup `
  -ServiceName SearchEngineBackupService `
  -DisplayName 'SearchEngine Backup Service' `
  -StartupType AutomaticDelayedStart `
  -Start
```

Скрипт не перезаписывает существующую службу, не выбирает Debug-бинарник по
умолчанию, задаёт description, recovery actions и preshutdown timeout.
Firewall не настраивается: BackupService не слушает сеть.

Рекомендуется один экземпляр с `Backup.all.json`. Не запускайте одновременно
объединённый профиль и перекрывающие его `Backup.databases.json` /
`Backup.programs.json`: одни и те же source/backup_dir будут обрабатываться
конкурирующими процессами.

## Несколько экземпляров

Каждому экземпляру нужны уникальные service name и желательно отдельный
data-dir:

```powershell
tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath C:\Backup\bin\BackupService.exe `
  -ConfigPath C:\Backup\Backup.databases.json `
  -DataDir C:\Backup\runtime-databases `
  -ServiceName SearchEngineBackupDatabases `
  -DisplayName 'SearchEngine Backup - Databases'

tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath C:\Backup\bin\BackupService.exe `
  -ConfigPath C:\Backup\Backup.programs.json `
  -DataDir C:\Backup\runtime-programs `
  -ServiceName SearchEngineBackupPrograms `
  -DisplayName 'SearchEngine Backup - Programs'
```

## Учётная запись и права

По умолчанию `New-Service` использует LocalSystem. Это приемлемо только для
локальных D:/F: путей при подходящих ACL. Для отдельной учётной записи
передайте `-Credential (Get-Credential)`.

Аккаунту нужны:

- Read/List/Traverse для всех `src`;
- Create/Write/Delete/Rename для каждого `backup_dir`;
- запись в `data-dir` и `logs`;
- чтение SQLite-файлов для Online Backup.

Mapped drive letters не видны службе и installer их отклоняет. Для UNC нужен
реальный UNC-путь и служебная/доменная учётная запись с сетевыми правами.

## Управление и диагностика

```powershell
Start-Service SearchEngineBackupService
Get-Service SearchEngineBackupService
Stop-Service SearchEngineBackupService
Restart-Service SearchEngineBackupService
sc.exe queryex SearchEngineBackupService
```

Удаление регистрации:

```powershell
tools\backup\scripts\Uninstall-BackupService.ps1 `
  -ServiceName SearchEngineBackupService
```

Uninstall не удаляет конфигурацию, логи, cache, snapshots или mirror history.
Ошибки конфигурации не позволяют службе достичь `RUNNING`; `sc.exe query` и
backup log показывают код/причину. Временная недоступность отдельного
`source`/`backup_dir` помечает запуск failed, но служба продолжает расписание.

## Остановка и аварийное завершение

При STOP, SHUTDOWN или PRESHUTDOWN служба:

1. переходит в `STOP_PENDING`;
2. запрещает новые периодические запуски и отменяет timers;
3. не прерывает уже выполняющийся `BackupEngine::runOnce()`;
4. ждёт `workers.join()`, обновляя SCM checkpoint и wait hint;
5. сообщает `STOPPED` только после завершения workers.

Большой активный снимок может заметно продлить остановку. Принудительное
завершение не используется. Публикация снимка выполняется переименованием
готового staging-каталога; предыдущий опубликованный снимок не повреждается.
После аварийного завершения оставшиеся `.partial_*` очищаются при следующем
запуске.
