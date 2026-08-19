# SearchEngine как Windows-служба

`SearchEngine.exe` поддерживает общий lifecycle для консоли и Windows Service:

```powershell
SearchEngine.exe --console
SearchEngine.exe --service
SearchEngine.exe --service --service-name SearchEngineService-archive
SearchEngine.exe --console --data-dir C:\SearchEngine\runtime
```

Без аргументов сохраняется консольный режим. Оба режима создают один
`SearchEngineApplication`, поэтому настройки, индекс, watcher, scheduler и
ASIO-сервер запускаются и останавливаются одной последовательностью.

По умолчанию сохраняются прежние имена:

- service name: `SearchEngineService`;
- display name: `Search Engine ASIO Server`.

Для нескольких экземпляров установщик задаёт уникальное service name через
`--service-name`. Значение должно совпадать с именем, зарегистрированным в SCM.

## Runtime-файлы и рабочий каталог

По умолчанию data-dir равен каталогу `SearchEngine.exe`. `--base-dir` является
синонимом `--data-dir`. Относительный путь разрешается от каталога exe, но для
службы рекомендуется передавать абсолютный путь.

До первого чтения настроек или создания лога процесс устанавливает data-dir
как текущий каталог. Это сохраняет совместимость существующего кода и не даёт
SCM создать файлы в `C:\Windows\System32`.

| Ресурс | Расположение |
|---|---|
| настройки сервера | `<data-dir>\Settings.json` |
| настройки backup-компонента | `<data-dir>\Backup.json` |
| таблица регистра OEM866 | `<data-dir>\OEM866.INI` |
| индекс | `<data-dir>\inverted_index.sqlite` |
| SQLite WAL/SHM | рядом с `inverted_index.sqlite` |
| журнал запросов | `<data-dir>\log.db` |
| старый server log | `<data-dir>\server_log.log` |
| структурированные логи | `<data-dir>\logs\...` |
| legacy messages state | `<data-dir>\messages` (historical, runtime does not use) |

Аккаунту службы нужны права создания/изменения файлов в data-dir, включая
SQLite WAL/SHM и `logs`. Каталог `messages` больше не используется runtime.

## Lifecycle и остановка

`SERVICE_RUNNING` сообщается только после проверки `Settings.json`, открытия
индекса и SQLite live-writer, запуска watcher/scheduler и успешного bind TCP
порта. При долгой загрузке индекса SCM получает обновляемые checkpoint и wait
hint.

STOP/SHUTDOWN handler только устанавливает stop-флаг и сигнализирует event.
Очистку выполняет `ServiceMain` в следующем порядке:

1. закрывает acceptor и активные TCP-сессии;
2. останавливает watchers и scheduler timers;
3. ждёт уже поставленные клиентские и scheduler-задачи;
4. ждёт индексирование и цепочки commit/CPU;
5. выполняет SQLite flush/checkpoint и останавливает writer;
6. разрушает объекты, содержащие ссылки на индекс;
7. сливает `SqlLogger`, завершает CPU pool и I/O-потоки;
8. сообщает `SERVICE_STOPPED` ровно один раз.

Остановка идемпотентна. Индексирование уже начатого файла не прерывается
принудительно, поэтому большой активный update может продлить `STOP_PENDING`.

В console mode команда `1`, Ctrl+C, Ctrl+Break, закрытие консоли и shutdown
Windows запускают ту же последовательность. В service mode не читается stdin,
не скрывается/показывается консоль, не вызываются MessageBox или `system("pause")`.

## Сборка Release

Из корня репозитория:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release `
  --target SearchEngine SearchEngineConfig AuthDbTool SearchClientTokenIssuer

cmake --preset windows7-x86
cmake --build --preset windows7-x86-release `
  --target SearchEngine SearchEngineConfig AuthDbTool SearchClientTokenIssuer
```

USB and computer tokens (`format_version` 1) are signed RS256 by `SearchClientTokenIssuer`.
Place `issuer-public.pem` in the service data directory (next to
`auth_clients.sqlite`), for example:

```powershell
.\SearchClientTokenIssuer.exe --export-public "$env:ProgramData\SearchEngineService"
```

If `issuer-public.pem` is absent, the server falls back to
`%ProgramData%\SearchClientTokenIssuer\keys\public.pem` from the issuer
keystore (same path `SearchClientTokenIssuer` uses by default).

The client checks the local device identity (`usb` hardware serial or
`computer` SMBIOS UUID); the server verifies the signature on `AUTHENTICATE_V1`.

Executable остаётся обычным console-subsystem приложением с `wmain`; custom
ENTRY и Windows GUI subsystem не используются.

## Переносимые установочные комплекты x64 и x86

Для установки на компьютере, где нет исходников, CMake и Visual Studio,
используется готовая папка. Сначала проверьте целевые пути в
`deployment\SearchEngineServicePortable\source-data\Settings.json`.

### Автоматически после Release-сборки цели

В packagable presets при `SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=ON` обычная
**Release** сборка/Rebuild цели:

1. **До компиляции** повышает patch app-версии продукта
   (`scripts\Ensure-ReleaseVersionBump.ps1` → `app-version*.json` и
   `cmake/generated/...` VERSIONINFO). Для SearchEngineService общая версия
   поднимается один раз до сборки и `SearchEngine.exe`, и
   `SearchEngineConfig.exe`.
2. После успешной линковки PostBuild вызывает
   `PostBuild-*Package.ps1` → `New-*Package.ps1` и упаковывает **ту же** новую
   версию (только текущий preset).

| Preset | Параметр packager | Папка пакета |
|---|---|---|
| `windows-x64` | `x64` | `SearchEngineService-x64` / `BackupService-x64` |
| `windows-x86` | `x86-modern` | `SearchEngineService-x86` / `BackupService-x86` |
| `windows7-x86` | `x86` | `*-x86-Windows7` (VS2019/v142, Win7 SP1) |

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --target SearchEngine
cmake --build --preset windows-x64-release --target BackupService

cmake --preset windows-x86
cmake --build --preset windows-x86-release --target SearchEngine

cmake --preset windows7-x86
cmake --build --preset windows7-x86-release --target SearchEngine
```

`ZagEditor` / `BackupRestore` — отдельные цели; тот же контракт bump-before-compile
+ PostBuild в packagable presets.
Debug/RelWithDebInfo/MinSizeRel версию не повышают и пакеты не публикуют.
Отключить bump и автоупаковку IDE-пути:
`-DSEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=OFF`.

`Build-*Package.ps1` сам делает один bump (или `-SkipVersionBump`), затем
configure с `PACKAGE_ON_RELEASE_BUILD=OFF` и/или
`SEARCHENGINE_VERSION_BUMP_MODE=skip`, чтобы не было второго повышения.
`New-*Package.ps1` только упаковывает текущую версию.

При `PACKAGE_ON_RELEASE_BUILD=ON` Release-сборка цели каждый раз запускает
bump-target (он всегда out-of-date), обновляет `.rc` и перелинковывает EXE, после
чего POST_BUILD упаковывает новую версию. Для упаковки без bump используйте
`New-*Package.ps1` или `Build-*Package.ps1 -SkipVersionBump`.

Если задан User/process env `WORKSPACE_RELEASE_CLOUD_ROOT`, после успешного
`New-*` ZIP + `.sha256` копируются на Google Drive. Отключение на один запуск
упаковщика: `-SkipCloudPublish`. Подробности:
`TOOLS\instructions\RELEASE_CLOUD_PUBLISH.md` и `docs/BUILDING_WINDOWS.md`.

### Ручная / полная упаковка

```powershell
.\scripts\Build-SearchEngineServicePackage.ps1
```

Скрипт последовательно пересобирает `SearchEngineConfig.exe` (`--clean-first`),
затем собирает `SearchEngine.exe` и остальные helper-цели для каждой
конфигурации. Перед упаковкой packager проверяет, что helper не старее
`tools/config/main.cpp`, и прогоняет контракт пустых AutoPad-путей
(`prm_base_dir` / `prd_base_dir` = `""` отключает источник). Только после
успешной Release-сборки и этих проверок формируются комплекты:

```text
out\package\SearchEngineService-x64\
out\package\SearchEngineService-x86-Windows7\
```

Современный x64-вариант собирается VS2022/v143 с целью Windows 10. Legacy x86
собирается отдельно VS2019/v142 с `WINVER` и `_WIN32_WINNT` равными `0x0601`,
комплектуется VC++ 2015-2019 Runtime 14.29 x86 и рассчитан на Windows 7 SP1 или
более новую Windows. Windows 7 RTM не поддерживается.

Чтобы собрать только один вариант:

```powershell
.\scripts\Build-SearchEngineServicePackage.ps1 -Architecture x64
.\scripts\Build-SearchEngineServicePackage.ps1 -Architecture x86
```

Если Release уже собран и пересобирать его не нужно:

```powershell
.\scripts\New-SearchEngineServicePackage.ps1
.\scripts\New-SearchEngineServicePackage.ps1 -Architecture x86
```

`New-SearchEngineServicePackage.ps1` не вызывает cmake: `SearchEngineConfig.exe`
в `out/build/<preset>/Release` должен быть свежее `tools/config/main.cpp`.
Иначе упаковка завершится ошибкой. Пустые `prm_base_dir` / `prd_base_dir`
должны проходить `SearchEngineConfig validate` — packager проверяет это
автоматически для четырёх комбинаций до копирования в `tools\`.

В комплект входят:

- `app\SearchEngine.exe`;
- `tools\SearchEngineConfig.exe` той же архитектуры;
- `data\Settings.json`, `OEM866.INI`, `ignore.txt` и пустые runtime-каталоги;
- соответствующий подписанный Microsoft VC++ Redistributable;
- BAT-скрипты установки, остановки, запуска, перезапуска и полного удаления;
- подробная `INSTALLATION_GUIDE_RU.txt` с назначением каждого файла;
- manifest с размерами и SHA-256 обязательных файлов.

Рабочий индекс, `log.db`, WAL/SHM и логи в комплект намеренно не копируются:
на новой машине это изменяемые данные, и служба создаёт их в своём data-dir.
Редактировать `data\Settings.json` в уже собранном комплекте разрешено — это
целевой конфиг конкретного компьютера.
Вся папка `data\` намеренно исключена из SHA-256-проверки пакета, включая
`OEM866.INI` и `ignore.txt`. JSON сформированных настроек валидируется
установщиком после диалога, а не по шаблону комплекта до установки.

Скопируйте **всю** выбранную папку комплекта на носитель, затем на целевой
компьютер. Запустите `Install-SearchEngineService.bat` от имени администратора.
На целевой машине PowerShell не нужен ни x86-, ни x64-варианту.

Установщик предлагает и проверяет:

- идентификатор экземпляра службы с подсказкой использовать разные имена для
  разных годов или разных наборов индексируемых папок;
- порт 1..65535;
- год 2000..2099;
- число исполнительных потоков (не 1 и не более двух логических CPU, с
  архитектурным пределом 32 для x86 и 64 для x64);
- таймаут индексации одного файла 10..600 секунд, рекомендация 120;
- `enable_prm_short_content_autodetect`, по умолчанию включённый.
- `document_catalog_storage`: `memory` по умолчанию либо `sqlite`; вопрос
  показывается и при переустановке, а импортированный режим становится
  предлагаемым значением;
- `tlg_send_root`, `razn_output_dir`, `opis_base_dir`, `f12_base_dir`:
  абсолютные локальные Windows-пути; физическое существование не требуется.

Служба portable-установщика работает как LocalSystem. User mapped drives из
обычной пользовательской сессии службе недоступны. Буква диска сама по себе
не доказывает, что это физический локальный том. Отсутствие F12/OPIS/tlg/razn
каталога не ломает startup: ошибка появляется при вызове функции.

Помощник атомарно формирует UTF-8 `Settings.json`, проверяет синтаксис путей и порт.
После запуска установщик требует настоящий ответ PONG, а не только состояние
SCM `RUNNING`.

На 64-разрядной Windows x86-комплект устанавливает программу в
`C:\Program Files (x86)\SearchEngineService`. Windows 7 x86 использует только
штатные `cmd.exe`, `sc.exe`, `reg.exe`, `netsh.exe`, `xcopy.exe`, `certutil.exe`
и 32-битный нативный помощник VS2019/v142.

При повторном запуске предлагается полный экспорт, экспорт настроек и логов
либо явно подтверждённая переустановка без экспорта. Program Files заменяется,
а ProgramData остаётся persistent state этого instance: индекс, авторизация,
historical `messages`, logs и неизвестные runtime-файлы не переносятся. Settings.json
собирается из нового template с импортом старых значений; OEM866.INI и
client-endpoint.txt имеют малый rollback. Пользовательский ignore.txt сохраняется.
Отказ от export больше не удаляет ProgramData после health-check. При ошибке
установщик возвращает старое приложение, managed-файлы, firewall/port и службу
без копии всего индекса.
`Uninstall-SearchEngineService.bat` использует тот же выбор экспорта и после
подтверждения полностью удаляет службу, firewall rule, приложение и data-dir.
При запуске без аргумента он перечисляет зарегистрированные
`SearchEngineService[-instance]` и предлагает выбрать удаляемый экземпляр.
Удаление Program Files, ProgramData и rollback-каталогов выполняется с
повторными попытками после `STOPPED` и отдельного ожидания завершения PID.
Занятый каталог можно повторно удалить после закрытия удерживающей программы.
Регистрация службы удаляется только после успешной очистки файлов. Если
регистрация уже отсутствует, но каталоги остались от старого или прерванного
удаления, установщик предлагает явно очистить их перед новой установкой.
Uninstall также ждёт исчезновения SCM/registry-регистрации после `sc delete`,
поскольку открытый `services.msc` может удерживать службу помеченной на
удаление. В x86-комплекте пути с `Program Files (x86)` обрабатываются через
`goto`-ветки: скобки в пути внутри `if (...)` ломают разбор `cmd.exe` и раньше
могли оборвать uninstall сразу после остановки службы. Последняя диагностика
сохраняется в `%TEMP%\SearchEngineService-Uninstall-last.log`, а финальное
окно требует явного нажатия `0`.

Специальная модификация `.zag` удалена из SearchEngine. Эту задачу выполняет
отдельная цель и программа `ZagEditor`.

## Установка

Официальный режим SearchEngineService — LocalSystem. Portable installer создаёт
службу без `obj=` / `password=`. Рабочие данные должны лежать на локальных
путях, доступных LocalSystem по ACL.

Установку portable-комплекта выполнять `Install-SearchEngineService.bat` от
имени администратора. PowerShell на целевой машине не нужен.

Отдельный developer-скрипт `scripts\Install-SearchEngineService.ps1` может
установить ту же службу для локальной разработки. Официальный режим — LocalSystem:

```powershell
.\scripts\Install-SearchEngineService.ps1 `
  -BinaryPath C:\SearchEngine\bin\SearchEngine.exe `
  -DataDir C:\SearchEngine\runtime `
  -UseLocalSystem
```

Скрипт проверяет Release-путь, обязательные runtime-файлы и основные поля
`Settings.json`, не перезаписывает существующую службу, включает recovery
restart и задаёт длительный preshutdown timeout. Firewall не меняется без
явного `-AddFirewallRule`.

### Несколько экземпляров на одном компьютере

Каждому экземпляру нужны уникальные `InstanceId`, `DataDir` и TCP-порт в его
`Settings.json`. Например:

```powershell
.\scripts\Install-SearchEngineService.ps1 `
  -BinaryPath C:\SearchEngine-main\SearchEngine.exe `
  -DataDir C:\ProgramData\SearchEngineService-main `
  -InstanceId main -UseLocalSystem -AddFirewallRule -Start

.\scripts\Install-SearchEngineService.ps1 `
  -BinaryPath C:\SearchEngine-archive\SearchEngine.exe `
  -DataDir C:\ProgramData\SearchEngineService-archive `
  -InstanceId archive -UseLocalSystem -AddFirewallRule -Start
```

Будут зарегистрированы `SearchEngineService-main` и
`SearchEngineService-archive` с различимыми display name. Переносимый пакет при
обычном запуске спрашивает instance id; `default` сохраняет старую
однослужбовую схему. Для автоматизации значение можно записать в
`ServiceInstance.cmd` или передать первым аргументом BAT, например
`Install-SearchEngineService.bat archive`.

Новый клиент не использует SCM-имя. В `servers` рекомендуется записать
`server_id=main/archive` и один host, а в `server_ports` — разные порты для
нужного года. Установщик сохраняет готовую подсказку в
`<data-dir>\client-endpoint.txt`, не изменяя клиентскую БД автоматически.

Для сетевых каталогов используйте UNC, а не mapped drive. Аккаунту нужны:

- Read/List/Traverse и `ReadDirectoryChangesW` для всех `config.dirs`;
- доступ к PRM/PRD/F12 и прочим путям бизнес-команд;
- Create/Write/Delete/Rename в data-dir;
- сетевые права и Kerberos/SMB-доступ, если используются UNC-пути.

## Управление

Штатная остановка и запуск — пара Stop/Start (новый процесс), а не Windows
Pause/Continue: служба их не поддерживает. После `Stop` → `Start`
`SearchEngineApplication::start()` заново читает `Settings.json` и
инициализирует runtime-ресурсы; отдельная перезагрузка конфигурации в C++ не
нужна.

Остановленная служба с Automatic / Delayed Start снова запустится после
перезагрузки Windows. Для долговременного отключения отдельно переведите
Startup Type в Manual или Disabled — скрипты Stop/Start это не делают.

Изменение порта в установленном `Settings.json` подхватывается новым процессом
и проверяется PING/PONG по актуальному порту. Правило Windows Firewall,
`client-endpoint.txt` и клиентская база службой и Start-скриптом автоматически
не обновляются: проверьте их вручную. Start выводит предупреждение при
расхождении порта.

```powershell
.\scripts\Stop-SearchEngineService.ps1
.\scripts\Start-SearchEngineService.ps1
.\scripts\Stop-SearchEngineService.ps1 -InstanceId archive
.\scripts\Start-SearchEngineService.ps1 -InstanceId archive
Get-Service SearchEngineService
sc.exe queryex SearchEngineService
.\scripts\Restart-SearchEngineService.ps1
.\scripts\Restart-SearchEngineService.ps1 -InstanceId archive
```

В переносимом комплекте без PowerShell:

```bat
Stop-SearchEngineService.bat
Start-SearchEngineService.bat
Stop-SearchEngineService.bat archive
Start-SearchEngineService.bat archive
Restart-SearchEngineService.bat
```

`Stop-SearchEngineService` ждёт штатный `STOPPED` до 1800 секунд и не предлагает
`taskkill`. `Start-SearchEngineService` проверяет установленный
`Settings.json`, дожидается `RUNNING` и выполняет PING/PONG; при уже
`RUNNING` готовность всё равно проверяется.

### Изменение конфигурации установленной службы (SVC-001)

Для безопасного редактирования production `Settings.json` используйте
`Configure-SearchEngineService.bat` из переносимого комплекта:

```bat
Configure-SearchEngineService.bat
Configure-SearchEngineService.bat archive
```

Без явного instance Configure использует общий picker установленных служб —
тот же механизм, что регистрация auth-client и uninstall:
`SearchEngineConfig choose-installed-instance --purpose configure`.
Picker перечисляет все текущие установленные SearchEngine instances;
количество экземпляров определяется динамически по SCM.
С аргументом (`archive`) instance используется напрямую, без picker.

Workflow (требует Administrator):
1. Если instance не задан аргументом — показывает picker через
   `SearchEngineConfig choose-installed-instance --purpose configure`.
2. Автоматически получает фактический `data-dir` из SCM ImagePath через
   `SearchEngineConfig inspect-installed` — не зависит от `%ProgramData%`.
   Разрешает relative `--data-dir` относительно каталога EXE.
3. Открывает временную копию `Settings.json` в Notepad; валидирует перед применением.
4. Останавливает службу (ожидание до 1800 секунд).
5. Атомарно заменяет `Settings.json` (и `client-endpoint.txt` при изменении порта)
   через `settings-transaction-apply`.
6. Обновляет правило брандмауэра (если требуется). Failure прерывает apply.
7. Запускает службу, проверяет `RUNNING` + PING/PONG на новом порту.
8. При успехе: `settings-transaction-commit` удаляет rollback-dir.

**Rollback contract (при любом failure после apply):**
- Файлы восстанавливаются **только** после подтверждённого SCM state = STOPPED.
  State-aware loop обрабатывает RUNNING/START_PENDING/STOP_PENDING корректно.
- Если STOPPED не достигнут за 1800 секунд — файлы НЕ восстанавливаются;
  rollback-dir сохраняется для ручного восстановления.
- Если `settings-transaction-rollback` завершился с ошибкой — commit не вызывается,
  rollback-dir сохраняется.
- Если restore правила брандмауэра не удался — rollback-dir сохраняется.
- `settings-transaction-commit` вызывается только после подтверждённого PING/PONG
  на старом порту. При failure commit — только предупреждение; здоровый новый конфиг
  не откатывается.

**Firewall:**
- Portable-installer rule (`SearchEngineService[-instance] TCP`): только localport.
- PowerShell-installer rule (`<display name> (<port>/TCP)`): удаляется и создаётся
  заново с новым именем и портом (имя включает порт), при этом сохраняется
  baseline, определённый установщиком репозитория: inbound allow, TCP port,
  привязка `SearchEngine.exe` (program) и enabled. Rollback симметричен.

**Ограничения:**
- Hot reload не поддерживается — всегда Stop→Start.
- Не изменяет индексы, auth-базу, логи и другие runtime-данные.
- Cyrillic-path round-trip через `cmd.exe` не верифицирован без integration test.
- Для PowerShell-style delete/recreate firewall-rule гарантируется сохранение
  только baseline параметров установщика (имя/display-name, inbound allow,
  TCP port, `SearchEngine.exe` program binding, enabled). Дополнительные
  вручную заданные свойства после установки не гарантированы к сохранению.
- При manual recovery: rollback-dir находится в `%TEMP%\SE-Configure-*`; скрипт
  выводит путь при любой неудаче.

Логи находятся под `<data-dir>\logs`; ранние ошибки запуска также отражаются
ненулевым SCM exit code. Для просмотра последних строк:

```powershell
Get-ChildItem C:\SearchEngine\runtime\logs -Recurse -Filter *.log |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 10 FullName, LastWriteTime
```

Удаление регистрации:

```powershell
.\scripts\Uninstall-SearchEngineService.ps1
.\scripts\Uninstall-SearchEngineService.ps1 -InstanceId archive
```

Uninstall сначала ждёт `STOPPED`, затем удаляет только SCM-регистрацию. Он не
удаляет `Settings.json`, индекс, WAL/SHM, базы, historical `messages` или логи.

## Диагностика

### Порт занят

Служба не достигает `RUNNING`, пишет `cannot bind ASIO listen port` и
возвращает код Winsock. Найдите владельца порта из `Settings.json`:

```powershell
Get-NetTCPConnection -LocalPort 15001 -State Listen |
  Select-Object LocalAddress, LocalPort, OwningProcess
Get-Process -Id <OwningProcess>
```

### Ошибка доступа

Проверьте identity и командную строку службы:

```powershell
Get-CimInstance Win32_Service -Filter "Name='SearchEngineService'" |
  Select-Object Name, StartName, PathName, State, ExitCode
```

Запустите проверку путей от имени того же аккаунта. Особое внимание уделите
ACL data-dir, индексируемым каталогам, UNC-ресурсам и антивирусной блокировке
`inverted_index.sqlite`, `-wal` или `-shm`.

### Settings.json отсутствует или повреждён

Служба остаётся `STOPPED` с ненулевым кодом. Исправьте файл в фактическом
data-dir из `PathName` службы; service mode не показывает диалоговые окна и не
продолжает работу с непроверенными обязательными полями.

См. также переносимый комплект BackupService:
[`tools/backup/WINDOWS_SERVICE.md`](../tools/backup/WINDOWS_SERVICE.md).
