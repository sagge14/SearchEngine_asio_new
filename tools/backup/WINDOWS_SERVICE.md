# BackupService как Windows-служба

`BackupService.exe` использует один и тот же `BackupServiceApplication` в
консоли и под Service Control Manager (SCM). Снимки, `mirror_history`,
SQLite Online Backup и формат `Backup.json` от режима запуска не зависят.

Поведение install/uninstall/reinstall/restart выровнено с эталоном
`SearchEngineService` (см. `docs/WINDOWS_SERVICE.md` и portable BAT в
`deployment/SearchEngineServicePortable/`). Отличия Backup: нет firewall
(служба не слушает сеть) и uninstall никогда не удаляет `backup_dir`
(снимки / mirror history).

## Режимы запуска и пути

```powershell
BackupService.exe --console --config C:\Backup\Backup.all.json --data-dir C:\Backup
BackupService.exe --once --config C:\Backup\Backup.all.json --data-dir C:\Backup
BackupService.exe --service --service-name SearchEngineBackupService `
  --config C:\Backup\Backup.all.json --data-dir C:\Backup
BackupService.exe --validate-config --config C:\Backup\Backup.all.json `
  --data-dir C:\Backup
```

- без режима или с `--console` запускается периодический console mode;
- `--once` выполняет каждую группу один раз;
- `--service` подключается к SCM и не использует stdin или console signals;
- `--validate-config` разбирает `Backup.json`, отклоняет mapped drive letters
  и предупреждает о UNC, не запуская scheduler и снимки (код `0` или `2`).

По умолчанию `data-dir` равен каталогу `BackupService.exe`, конфигурация —
`<data-dir>\Backup.json`, логи — `<data-dir>\logs`. Относительный
`--data-dir` разрешается от каталога exe, относительный `--config` — от
`data-dir`. Относительные `src` и `backup_dir` внутри JSON разрешаются от
каталога конфигурации. Поэтому SCM не создаёт рабочие файлы в
`C:\Windows\System32`.

## Канонические имена portable-установки

| Что | Значение |
|---|---|
| Service name | `SearchEngineBackupService` |
| Display name | `SearchEngine Backup Service` |
| Description | `Scheduled snapshot and SQLite backup service` |
| Install root | `%ProgramFiles%\SearchEngineBackupService` (x86 → Program Files (x86)) |
| Installed bin | `%INSTALL_ROOT%\bin\BackupService.exe` |
| Data dir | `%ProgramData%\SearchEngineBackupService` |
| Config | `%DATA_DIR%\Backup.json` |

binPath:

```text
"<InstallRoot>\bin\BackupService.exe" --service --service-name "SearchEngineBackupService" --config "<DataDir>\Backup.json" --data-dir "<DataDir>"
```

## Сборка

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --target BackupService

cmake --preset windows-x86
cmake --build --preset windows-x86-release --target BackupService

# Windows 7 SP1 x86 (VS2019 v142)
cmake --preset windows7-x86
cmake --build --preset windows7-x86-release --target BackupService
```

Используйте Release-бинарник той разрядности, которая нужна на машине.
Служба остаётся console-subsystem executable; нестандартной точки входа нет.

## Переносимый комплект (целевые машины без PowerShell)

Шаблоны: `deployment/BackupServicePortable/`.

В presets `windows-x64` / `windows-x86` / `windows7-x86` при
`SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=ON` Release-сборка/Rebuild цели
`BackupService` сначала повышает patch (`Ensure-ReleaseVersionBump.ps1`), затем
после успешной линковки вызывает `PostBuild-BackupServicePackage.ps1` →
`New-BackupServicePackage.ps1` (текущий preset) и при заданном
`WORKSPACE_RELEASE_CLOUD_ROOT` публикует ZIP на Drive. Отключение bump и
автоупаковки: `-DSEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=OFF`. Подробности:
`docs/BUILDING_WINDOWS.md`.

Ручная сборка пакета на машине разработчика:

```powershell
.\scripts\Build-BackupServicePackage.ps1 -Architecture x64
.\scripts\Build-BackupServicePackage.ps1 -Architecture x86
# или оба: -Architecture All
```

По умолчанию в `data\Backup.json` кладётся шаблон
`deployment/BackupServicePortable/source-data/Backup.json`
(экономный AutoPad / `mirror_history`). Классический snapshot-профиль:

```powershell
.\scripts\Build-BackupServicePackage.ps1 -Architecture x64 `
  -ConfigPath tools\backup\configs\autopad\Backup.all.json
```

Результат: `out/package/BackupService-x64` или
`out/package/BackupService-x86-Windows7`. На целевом компьютере скопируйте
всю папку, отредактируйте `data\Backup.json`, затем запустите
`Install-BackupService.bat` от имени администратора.

Состав пакета: `app\`, `data\`, `prerequisites\`,
`Install/Stop/Start/Restart/Uninstall/Verify-Package.bat`,
`ServiceInstance.cmd`, README и INSTALLATION_GUIDE_RU.

Установка кладёт программу в `Program Files\SearchEngineBackupService\bin`,
конфиг и логи — в `C:\ProgramData\SearchEngineBackupService`. Каталоги
снимков (`backup_dir`) в пакет не входят и uninstall их не удаляет.
Firewall не настраивается.

Проверка комплекта без установки: `Install-BackupService.bat /validate`.

### Поведение portable Install / Reinstall

Порядок как у SearchEngineService:

1. SHA-256 verify + `--validate-config`
2. leftover-каталоги при отсутствии службы → явное предложение удалить
   (`DELETE_DIRECTORY_RETRY`) или отменить
3. reinstall: CHOOSE_BACKUP → stop → export settings/logs → move-to-rollback →
   copy → `sc create`/`sc config` → start → wait RUNNING → удалить rollback
4. Stop/Restart/Uninstall scripts: выбор `Graceful` / `Immediate` (или аргумент
   `graceful`/`immediate`); Immediate делает STOP + точечный kill только
   повторно проверенного PID; после STOPPED Restart ждёт выхода прежнего PID
5. Program Files (x86): пути со скобками только через goto-ветки

### Поведение portable Uninstall

1. diagnostic log: `%TEMP%\SearchEngineBackupService-Uninstall-last.log`
2. CHOOSE_BACKUP (settings/logs или none) → confirm
3. выбор StopMode (`Graceful`/`Immediate`) → stop + wait STOPPED + wait PID exit
4. export (если выбран)
5. `DELETE_DIRECTORY_RETRY` app + ProgramData
6. удаление orphaned `.rollback-*`
7. `DELETE_SERVICE_RETRY` (**после** файлов; poll registry)
8. `WAIT_BEFORE_CLOSE` (Press 0)

Снимки и mirror history под `backup_dir` не трогаются.

Скрипты `tools/backup/scripts/*.ps1` остаются для разработки и админской
установки с уже готовыми путями; на целевых машинах без PowerShell используйте
только BAT из пакета.

## Установка одного экземпляра (PowerShell)

Из PowerShell с правами администратора. Нужно явно указать либо
`-UseLocalSystem`, либо `-Credential` (как у SearchEngineService):

```powershell
tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath F:\SearchEngine_asio_new\out\build\windows-x64\Release\BackupService.exe `
  -ConfigPath C:\Backup\Backup.all.json `
  -DataDir C:\Backup `
  -ServiceName SearchEngineBackupService `
  -DisplayName 'SearchEngine Backup Service' `
  -StartupType AutomaticDelayedStart `
  -UseLocalSystem `
  -Start
```

Скрипт не перезаписывает существующую службу, не выбирает Debug-бинарник по
умолчанию, задаёт description, recovery actions и preshutdown timeout (30 мин).
Firewall не настраивается: BackupService не слушает сеть.

Остановка, запуск и перезапуск:

```powershell
tools\backup\scripts\Stop-BackupService.ps1
tools\backup\scripts\Stop-BackupService.ps1 -InstanceId default -StopMode Graceful
tools\backup\scripts\Stop-BackupService.ps1 -InstanceId default -StopMode Immediate
tools\backup\scripts\Start-BackupService.ps1
tools\backup\scripts\Restart-BackupService.ps1 -InstanceId default -StopMode Graceful
```

`Stop` / `Start` — штатные команды SCM, не Pause/Continue. После
Stop → Start создаётся новый процесс, и `BackupServiceApplication::configure()`
заново читает установленный `Backup.json` (обычно
`C:\ProgramData\SearchEngineBackupService\Backup.json`). Отдельная доработка
C++ для перезагрузки конфигурации не нужна. Редактируйте установленный файл в
ProgramData; шаблон `data\Backup.json` в portable-комплекте уже установленную
службу не меняет.

Скрипты управления поддерживают два режима остановки (`-StopMode`):

| Режим | Поведение |
|---|---|
| `Graceful` (по умолчанию) | Штатный `STOP`: запрет новых задач, отмена timers, ожидание `BackupEngine::runOnce()`, затем `SERVICE_STOPPED`. Без `taskkill` / `Stop-Process -Force`. |
| `Immediate` | Сначала обычный `STOP` (прекращение планирования), короткий `-ImmediateGraceSeconds` (по умолчанию 2), затем точечный `Stop-Process -Force` только для повторно проверенного PID экземпляра. |

Если `-StopMode` не указан и консоль интерактивна, скрипт показывает выбор
`[1] Graceful` / `[2] Immediate` / `[0] Отмена` (Enter = Graceful). В
неинтерактивном режиме без параметра всегда выбирается Graceful.
`services.msc`, `SERVICE_CONTROL_SHUTDOWN` и `SERVICE_CONTROL_PRESHUTDOWN`
всегда инициируют Graceful внутри службы; диалогов из Session 0 нет.

Риски Immediate: активный backup прерывается; незавершённый staging
(`.partial_*`) может остаться до следующего запуска, который очищает такие
каталоги. Опубликованные snapshots, cache и конфигурация не удаляются.

Служба с Automatic/Delayed Start снова запустится после перезагрузки; для
долговременного отключения отдельно смените Startup Type.

Рекомендуется один экземпляр с economical-профилем (портативный пакет) или
`Backup.all.json` (полные snapshot). Не запускайте одновременно
объединённый профиль и перекрывающие его `Backup.databases.json` /
`Backup.programs.json`: одни и те же source/backup_dir будут обрабатываться
конкурирующими процессами.

## Несколько экземпляров

Каждому экземпляру нужны уникальные service name и желательно отдельный
data-dir (PowerShell-путь; portable BAT ставит канонический
`SearchEngineBackupService`):

```powershell
tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath C:\Backup\bin\BackupService.exe `
  -ConfigPath C:\Backup\Backup.databases.json `
  -DataDir C:\Backup\runtime-databases `
  -ServiceName SearchEngineBackupDatabases `
  -DisplayName 'SearchEngine Backup - Databases' `
  -UseLocalSystem

tools\backup\scripts\Install-BackupService.ps1 `
  -BinaryPath C:\Backup\bin\BackupService.exe `
  -ConfigPath C:\Backup\Backup.programs.json `
  -DataDir C:\Backup\runtime-programs `
  -ServiceName SearchEngineBackupPrograms `
  -DisplayName 'SearchEngine Backup - Programs' `
  -UseLocalSystem
```

## Учётная запись и права

`-UseLocalSystem` приемлем только для локальных D:/F: путей при подходящих
ACL. Для отдельной учётной записи передайте `-Credential (Get-Credential)`.

Аккаунту нужны:

- Read/List/Traverse для всех `src`;
- Create/Write/Delete/Rename для каждого `backup_dir`;
- запись в `data-dir` и `logs`;
- чтение SQLite-файлов для Online Backup.

Mapped drive letters не видны службе и installer их отклоняет. Для UNC нужен
реальный UNC-путь и служебная/доменная учётная запись с сетевыми правами.

## Управление и диагностика

```powershell
tools\backup\scripts\Stop-BackupService.ps1
tools\backup\scripts\Stop-BackupService.ps1 -InstanceId default -StopMode Graceful
tools\backup\scripts\Stop-BackupService.ps1 -InstanceId default -StopMode Immediate
tools\backup\scripts\Start-BackupService.ps1
tools\backup\scripts\Stop-BackupService.ps1 -InstanceId myinstance -StopMode Graceful
tools\backup\scripts\Start-BackupService.ps1 -InstanceId myinstance
Get-Service SearchEngineBackupService
sc.exe queryex SearchEngineBackupService
tools\backup\scripts\Restart-BackupService.ps1 -InstanceId default -StopMode Immediate
```

В переносимом комплекте без PowerShell:

```bat
Stop-BackupService.bat
Stop-BackupService.bat graceful
Stop-BackupService.bat immediate
Stop-BackupService.bat myinstance graceful
Start-BackupService.bat
Restart-BackupService.bat immediate
```

`Stop-BackupService` в режиме Graceful ждёт штатный `STOPPED` до 1800 секунд и
не выполняет force-kill. Immediate сначала отправляет `STOP`, затем при
необходимости завершает только повторно проверенный PID выбранного экземпляра
(`taskkill /PID ... /F`, никогда `/IM BackupService.exe`).
`Start-BackupService` дожидается `RUNNING`; при уже `RUNNING` завершается
успешно. `Restart` не стартует новый процесс, пока прежний PID ещё существует.
При ошибке скрипты показывают `sc query` и путь к логам.

Удаление регистрации (данные сохраняются):

```powershell
tools\backup\scripts\Uninstall-BackupService.ps1 `
  -InstanceId default -StopMode Graceful
```

PS Uninstall применяет тот же `StopMode` перед снятием регистрации и не
удаляет конфигурацию, логи, cache, snapshots или mirror history.
Полное удаление app + ProgramData — только portable `Uninstall-BackupService.bat`.

Ошибки конфигурации не позволяют службе достичь `RUNNING`; `sc.exe query` и
backup log показывают код/причину. Временная недоступность отдельного
`source`/`backup_dir` помечает запуск failed, но служба продолжает расписание.

## Остановка и аварийное завершение

### Graceful (поведение службы и режим по умолчанию)

При STOP из `services.msc`, SHUTDOWN или PRESHUTDOWN служба всегда выполняет
Graceful-путь (диалогов из Session 0 нет):

1. переходит в `STOP_PENDING`;
2. запрещает новые периодические запуски и отменяет timers;
3. не прерывает уже выполняющийся `BackupEngine::runOnce()`;
4. ждёт `workers.join()`, обновляя SCM checkpoint и wait hint;
5. сообщает `STOPPED` только после завершения workers.

Большой активный снимок может заметно продлить остановку. Пользовательские
скрипты `Stop` / `Restart` / `Uninstall` в режиме Graceful ждут `STOPPED` до
1800 с и не выполняют force-kill.

### Immediate (только скрипты управления)

Режим Immediate выбирается в PowerShell (`-StopMode Immediate`) или BAT
(`immediate` / пункт меню `[2]`). Служба получает обычный STOP, затем скрипт
после короткой паузы может принудительно завершить только повторно
проверенный PID этого экземпляра (`SERVICE_WIN32_OWN_PROCESS`). Нельзя
использовать широкий `taskkill /IM BackupService.exe`.

Риски: текущий backup прерывается; staging `.partial_*` может остаться до
следующего запуска. Опубликованные snapshots, cache и конфигурация не
удаляются. После аварийного/принудительного завершения оставшиеся
`.partial_*` очищаются при следующем запуске.

Публикация снимка выполняется переименованием готового staging-каталога;
предыдущий опубликованный снимок не повреждается.
