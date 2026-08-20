# SearchEngine — HANDOFF: аудит команд и работы в режиме Windows-службы

## Назначение

Рабочий реестр проблем, решений и сознательно принятых ограничений, обнаруженных
при переводе `SearchEngine.exe` в основной режим Windows-службы.

Пункты разбираются по одному. Сначала фиксируется решение, затем при необходимости
выполняется отдельная реализация и проверка.

---

## Контекст аудита

```text
server: sagge14/SearchEngine_asio_new
branch: main
server audit baseline: 59c03b25799da0435edda5ecafd9b3f764a9cfa3

client: myLitleWork/SearchEngine-client
branch: main
client audit baseline: 6c2167a2fb885f921c89b40f23e687ab0fef8a46

final cross-repo audit: 2026-08-19 CLEAN
```

После существенных изменений service lifecycle, filesystem/protocol, installer,
auth или AutoPad выводы необходимо перепроверять по актуальному HEAD.

---

## Статусы

```text
OPEN        — проблема зафиксирована, решение не принято
ANALYSIS    — идёт отдельный разбор
DECIDED     — решение принято, реализация ещё не выполнена/не завершена
DEFERRED    — сознательно отложено
REJECTED    — обсуждено; текущее поведение принято, исправление не требуется
CANCELLED   — пункт отменён
IN_PROGRESS — выполняется реализация
FIXED       — код изменён, итоговая проверка ещё не завершена
VERIFIED    — реализовано и проверено
```

---

## Краткий реестр

| ID | Приоритет | Тема | Статус |
|---|---:|---|---|
| SVC-001 | P1 | Runtime-root и редактирование настроек | VERIFIED |
| SVC-002 | P0/P1 | Service account и доступ к рабочим путям | VERIFIED |
| SVC-003 | P0 | `prefix_map.json` для `GET_ATTACHMENTS` при установке | VERIFIED |
| SVC-004 | P1/P2 | Семантика `GET_ATTACHMENTS` | VERIFIED |
| SVC-005 | P0 | Update/reinstall теряет runtime-state | VERIFIED |
| SVC-006 | P0/P1 | `PING/PONG` как core health-check | REJECTED |
| SVC-007 | P0 | Отсутствующий root воспринимается как отсутствие файлов | REJECTED |
| SVC-008 | P1 | Watcher может не подхватить поздно появившийся parent/root | REJECTED |
| SVC-009 | P0 security | `GETBINFILE`/`FILETEXT` принимают raw server paths | VERIFIED |
| SVC-010 | P0 security | Upload path escape и upload целиком в RAM | VERIFIED |
| SVC-011 | P1 | Жёсткие production paths `D:\...` | VERIFIED |
| SVC-012 | P1 | Основной download приложений через `GETBINFILE` | VERIFIED |
| SVC-013 | P2 | Legacy state и невидимые `cout/cerr` | VERIFIED |
| SVC-014 | P2 | Enum команд шире реально поддерживаемого registry | VERIFIED |

---

# Принятые решения

## SVC-005 — ProgramData является persistent state

**Статус:** VERIFIED

```text
Program Files = заменяемое приложение.
ProgramData   = постоянное состояние конкретного экземпляра службы.
```

Обычный update/reinstall того же instance не должен перемещать, удалять или
пересоздавать весь `%ProgramData%\SearchEngineService[-instance]`.

Сохранять:

- `auth_clients.sqlite`;
- `issuer-public.pem`;
- `inverted_index.sqlite` и связанные WAL/SHM;
- `messages\`;
- `prefix_map.json`;
- пользовательский `ignore.txt`;
- `log.db`, `server_log.log`, `logs\`;
- неизвестные operator-added runtime files.

`Settings.json` при update формируется из нового template с импортом старых
значений через существующий `configure-interactive --template ... --import-settings ...`
и заменяется атомарно.

`client-endpoint.txt` можно пересоздавать. `OEM866.INI` считается package-managed
resource и может обновляться, сохраняя rollback-копию.

Rollback откатывает прежде всего `Program Files` и небольшие installer-owned
config/auth-файлы. Копировать многогигабайтный индекс перед каждым update не
требуется.

При обычном update/reinstall автоматического destructive cleanup ProgramData
нет. Historical `messages\` сохраняются; runtime их больше не использует и
автоматически не удаляет.

Полное удаление ProgramData допускается только через явно подтверждённый
destructive uninstall/reset/leftover-cleanup workflow (включая случай, когда SCM
service отсутствует и оператор подтверждает удаление остатков).

---

## SVC-003 — `prefix_map.json` входит в release и настраивается installer

**Статус:** VERIFIED

`GET_ATTACHMENTS` остаётся рабочей функцией. Активный файл в service mode:

```text
%ProgramData%\SearchEngineService[-instance]\prefix_map.json
```

В release входит шаблон/заготовка `data\prefix_map.json`.

Installer спрашивает, используется ли на этом instance `GET_ATTACHMENTS` /
кнопка «Сохранить приложения».

- Если нет — отсутствие `prefix_map.json` не блокирует установку.
- Если да — проверить package-файл, валидировать JSON (`prefix` + `map`) и
  скопировать его при новой установке. При отсутствии — явное предупреждение,
  но не обязательный abort.
- При update существующий рабочий ProgramData `prefix_map.json` сохраняется и
  не перезаписывается шаблоном молча.

---

## SVC-004 — текущая семантика `GET_ATTACHMENTS` сохраняется

**Статус:** VERIFIED

Оставить текущий сценарий:

1. клиент вызывает `GET_ATTACHMENTS` только на `primaryServerId()`;
2. сервер по оператору и `prefix_map.json` находит буферный каталог;
3. читает его файлы;
4. после успешного чтения удаляет исходный буферный каталог;
5. возвращает содержимое клиенту.

Не вводить сейчас ACK/quarantine/processed-folder. Риск удаления буфера до
окончательной доставки принят как часть текущей legacy-семантики.

Final cross-repo audit на baseline `59c03b257` / `6c2167a2` подтвердил, что
production server/client соответствуют этому destructive-контракту.

---

## SVC-007 — отсутствие месячных папок: текущее поведение принято

**Статус:** REJECTED

В `Settings.json` нормально заранее перечислить все месяцы:

```text
D:\ЯНВАРЬ
...
D:\ДЕКАБРЬ
```

даже если будущие месяцы ещё физически не существуют.

Не вводить сейчас отдельные состояния для disconnected disk, network drive,
missing volume letter, Access Denied и т.п. Если root не найден и scan видит
его как пустой, документы могут быть помечены deleted; при повторном появлении
root они индексируются обратно.

Возможный будущий отдельный feature — `freeze` индекса, при котором поиск
работает по текущему индексу, а scanner/watcher его не изменяют.

---

## SVC-008 — watcher и позднее появление parent/root: текущее поведение принято

**Статус:** REJECTED

Нормальный сценарий, когда parent (например `D:\`) существует, а configured
месячная папка появляется позже, уже подхватывается parent watcher: создаётся
inner `FileWatcher` с `forceWalk=true`, который рекурсивно выдаёт Added для уже
находящихся там файлов.

Остаётся edge case: сам parent/root отсутствовал при старте и позднее появился
уже с существующими configured children. После открытия parent текущий код не
делает явного повторного enumerate kids и может ждать будущего directory event.

Этот edge case сейчас сознательно не исправляется. Решение согласовано с SVC-007:
сложную recovery-логику, дополнительные состояния и изменения production watcher
не вводить. Отдельное восстановление после позднего появления самого диска/root
может быть реализовано в будущем как самостоятельная feature.

---

## SVC-009 + SVC-012 — scoped-доступ к тексту и приложениям, end-to-end streaming

**Статус:** VERIFIED

Фактический итог:

```text
telegram text:
  GET_TELEGA_TEXT=32 {id,type}

PDTV confirmation:
  GET_ISH_PDTV_TEXT=33 {id,slot,entry_index}

GET_ISH_PDTV:
  strict integer id + read-only AutoPad lookup

attachments:
  GET_TELEGA_ATACHMENTS
      ->
  GET_TELEGA_SINGLE_ATACHMENT
  with server/client streaming

FILETEXT=2:
  legacy/reserved, server rejects raw request (InvalidCommand)

GETBINFILE=11:
  legacy/reserved, server rejects raw request (InvalidCommand)

client does not supply a physical server path for these production downloads
```

Все загрузки приложений переведены на безопасную схему:

```text
GET_TELEGA_ATACHMENTS
        ->
GET_TELEGA_SINGLE_ATACHMENT
```

Это касается и одиночного скачивания, и массового сохранения выбранных документов.

`GET_TELEGA_SINGLE_ATACHMENT` должен стать настоящим end-to-end streaming:
сервер читает файл блоками и передаёт блоками; SearchClient принимает блоками и
сразу пишет на диск. Большой файл не должен целиком находиться в RAM.

Контракт приложений:

```text
list:   id + type
single: id + type + file_name
```

Клиент не задаёт физический server path. Сервер сам через AutoPad определяет
`DirectTo` и допустимый файл.

Для текста телеграммы создать отдельный scoped endpoint/command по `id + type`:
сервер сам определяет `DirectTo/FileName` и возвращает содержимое. Клиент не
знает и не передаёт server filesystem path.

После миграции SearchClient старые raw-path `FILETEXT` и `GETBINFILE` запрещаются
для клиентского использования. Их wire ordinals не удалять и не перенумеровывать:
оставить legacy/reserved, а сервер должен отклонять raw-path requests.

Локальный путь, куда пользователь сохраняет файл на своём ПК, остаётся только на
стороне SearchClient.

Итоговый принцип:

```text
Клиент передаёт бизнес-идентификаторы.
Сервер сам определяет физические пути.
Большие файлы никогда целиком не загружаются в RAM.
Raw server filesystem paths через клиентский протокол больше не принимаются.
```

---

## SVC-010 — безопасная server-side маршрутизация upload и streaming

**Статус:** VERIFIED

Фактический итог:

```text
UPLOAD_TLG_TO_SEND_V1 = 34  (streaming V1 — active)
UPLOAD_RAZN_V1        = 35  (streaming V1 — active)

LOAD_TLG_TO_SEND = 15  (legacy/reserved, server rejects: InvalidCommand)
LOAD_RAZN        = 22  (legacy/reserved, server rejects: InvalidCommand)
```

Сервер отклоняет любой запрос с командой 15 или 22 до выполнения handler-а,
возвращая typed `InvalidCommand` с диагностикой
`"legacy upload is disabled; use streaming V1"`.
Wire ordinals 15 и 22 сохранены (не удалены, не перенумерованы).

`LOAD_TLG_TO_SEND` и `LOAD_RAZN` больше не должны использовать клиентский
`filename` как относительный server path и не должны передавать весь файл внутри
сериализованного `FileData`/`vector<uint8_t>`.

### `LOAD_RAZN`

Клиент передаёт только basename + metadata/stream файла. Каталоги, `..`, rooted,
absolute и UNC path запрещены. Destination определяет сервер.

### `LOAD_TLG_TO_SEND`

Клиент передаёт basename + metadata/stream. Имя оператора сервер берёт из уже
авторизованной сессии, а не из `<user>\filename` клиента.

Сервер сам строит бизнес-структуру:

```text
<tlg_send_root>\<МЕСЯЦ>\<ДАТА>\<authenticated_operator>_<time>\<filename>
```

Operator component должен быть безопасным одиночным filesystem-компонентом.

### Containment

Перед записью обязательна проверка, что итоговый destination остаётся внутри
command-specific root. Не допускать escape через `..`, absolute/rooted/UNC,
mixed separators, junction/symlink/reparse point.

### Existing file policy

Удалить правило `одинаковый размер = тот же файл`.

```text
имя свободно       -> сохранить;
имя занято         -> file(1).ext, file(2).ext, ...;
```

Существующие файлы автоматически не перезаписывать.

### End-to-end streaming upload

```text
client disk -> small blocks -> TCP -> small blocks -> server staging file
```

Большой upload не должен целиком находиться в RAM клиента, serialized request,
server `requestData` или `FileData::data`. Для этих команд нужен отдельный
streaming framing/path, потому что текущий общий `readLoop` сначала полностью
читает `requestHeader.size`.

Перед потоком передаётся ограниченная metadata (`filename`, `file size`, при
необходимости version/flags), сервер валидирует её и открывает staging file.
Публикация/rename выполняется только после полного успешного получения. При
обрыве partial/staging удаляется.

Wire ordinals не перенумеровывать; конкретный migration framing согласовать в
SearchEngine и SearchClient.

---

## SVC-002 — service account: production portable contract = LocalSystem

**Статус:** VERIFIED

Production portable package contract:

```text
SearchEngineService runs as LocalSystem.
Portable installer явно показывает:
  Service account: LocalSystem
и не поддерживает user mapped drives.
```

На текущем этапе официальный эксплуатационный контракт portable release:

```text
SearchEngineService работает как LocalSystem.
Рабочие данные находятся на локальных физических дисках/путях,
доступных этому service account по ACL.
```

Generic `scripts/Install-SearchEngineService.ps1` имеет отдельный explicit
выбор `-Credential` или `-UseLocalSystem`; это не меняет production portable
contract и не расширяет его на user mapped drives / UNC без отдельной задачи.

Portable installer сейчас не добавляет:

- выбор service account в interactive portable flow;
- логин/пароль службы;
- автоматический `net use`;
- поддержку user mapped drives;
- `subst`/profile-specific drive mappings;
- хранение credentials для UNC/network shares.

Если позже понадобится сетевое хранилище, это отдельная задача по service
account/UNC и credential model.

Portable installer/config diagnostics должны явно показывать:

```text
Service account: LocalSystem
```

и документация не должна создавать впечатление, что пользовательские mapped
drives автоматически видны production portable service.

Отсутствие/недоступность будущих monthly roots не является startup-fatal само по
себе — это остаётся решением SVC-007.

---

## SVC-011 — все скрытые production roots выносятся в Settings

**Статус:** VERIFIED

Жёсткие production paths в C++ убрать. В `Settings.json` добавить отдельные
настройки назначения, как минимум:

```json
{
  "tlg_send_root": "D:\\",
  "razn_output_dir": "D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ",
  "opis_base_dir": "D:\\OPIS_ADMIN",
  "f12_base_dir": "D:\\F12"
}
```

Не сводить всё в один общий `base_dir`: назначения разные и должны оставаться
явными.

Сервер сам строит конкретные пути:

```text
tlg_send_root
  -> <МЕСЯЦ>\<ДАТА>\<operator>_<time>\<file>

razn_output_dir
  -> <file>

opis_base_dir
  -> <year>.db / <year>.DB по фактическому контракту соответствующего consumer

f12_base_dir
  -> <year>.db
  -> base.db
```

Перевести на эти настройки текущие hardcoded consumers:

- `LOAD_TLG_TO_SEND` / live `UPLOAD_TLG_TO_SEND_V1=34` (15 reserved/rejected);
- `LOAD_RAZN` / live `UPLOAD_RAZN_V1=35` (22 reserved/rejected);
- `GET_OPIS_BASE`;
- `RecordProcessor`;
- `GET_VH_TELEGA_WAY` / `GET_ISH_TELEGA_WAY` (`TelegaWay`).

Существующие `dirs`, `prm_base_dir`, `prd_base_dir` остаются отдельными
настройками как сейчас.

Installer/config helper должен включить новые поля в template, interactive
configuration/import/validation. При update старые значения сохраняются по
правилу SVC-005, а новые поля получают template/default значения до явного
изменения оператором.

### Проверка доступности

Не требовать физического существования всех `dirs` при установке/startup: будущие
месячные папки могут отсутствовать.

Для feature-specific roots (`opis_base_dir`, `f12_base_dir`, `razn_output_dir`,
`tlg_send_root`) отсутствие или недоступность должна диагностироваться при
использовании соответствующей функции и возвращаться как нормальная typed error,
а не превращаться в доступ к скрытому fallback `D:\...`.

---

## SVC-006 — `PING/PONG` остаётся core readiness-check installer

**Статус:** REJECTED

Текущее `SCM RUNNING + PING/PONG` считается достаточным core readiness-check для
install/update/rollback.

Причина: SearchEngine начинает принимать TCP/PING только после успешного прохождения
основного startup-пути. К этому моменту уже должны быть загружены и провалидированы
`Settings.json`, инициализирован runtime/OEM, открыт auth store, создан SearchServer
и индекс, запущены watcher/scheduler, после чего создаётся `AsioServer`. Если core
startup ломается раньше, получить `PONG` нельзя.

Глобальный health **не должен** требовать доступности всех feature-specific
источников и каталогов. В частности его не должны ломать:

- отключённые/optional PRM/PRD sources;
- `prefix_map.json`, если `GET_ATTACHMENTS` не используется;
- F12/OPIS, если соответствующая функция сейчас не вызывается;
- `tlg_send_root` / `razn_output_dir`, если upload-функция не используется;
- отсутствующие будущие monthly roots из `dirs`.

Feature-specific path/source ошибки проверяются при вызове соответствующей функции
и возвращаются как typed error.

### Будущая диагностика — отдельная feature

В будущем можно добавить отдельную информационную диагностическую команду/tool,
которая показывает состояние компонентов, например:

```text
Core: OK
PRM: OK
PRD: disabled
F12: unavailable
OPIS: OK
GET_ATTACHMENTS: not configured
```

Такая диагностика предназначена для оператора и troubleshooting. Она **не должна**
становиться обязательным критерием успешной установки/rollback и не должна
превращать optional/будущие monthly roots в startup failure.

---

# Реализованные пункты

## SVC-001 — runtime-root службы и управление настройками

**Приоритет:** P1  
**Статус:** VERIFIED

### Реализованный контракт

Безопасный workflow изменения production `Settings.json` установленной службы
реализован через:

1. **`SearchEngineConfig inspect-installed [--instance ID]`** — получает фактический
   `--data-dir` из SCM ImagePath (не из `%ProgramData%`) и возвращает
   `instance=`, `service_name=`, `data_dir=`, `settings_path=`, `endpoint_path=`,
   `installed_program_path=` в формате `key=value` stdout.

2. **`choose-installed-instance --purpose configure`** — интерактивный выбор
   установленного instance для конфигурирования.

3. **`settings-transaction-apply --data-dir DIR --settings-temp FILE --rollback-dir DIR [--endpoint-temp FILE]`** —
   атомарный apply: snapshot `Settings.json` (обязателен) + `client-endpoint.txt`
   (опционально), затем atomic replacement через staging+MoveFileExW. Rollback-dir
   может находиться в `%TEMP%` (без ограничения sibling). Только управляемые файлы
   изменяются; sentinel-файлы (index, auth, logs, messages) не затрагиваются.

4. **`settings-transaction-rollback --data-dir DIR --rollback-dir DIR`** —
   побайтовое восстановление snapshot обратно в data-dir.

5. **`settings-transaction-commit --data-dir DIR --rollback-dir DIR`** —
   удаление rollback-dir после успешного PING/PONG.

6. **`validateJson()` (расширена и скорректирована)** — type/range checks для полей,
   выровненные с runtime `ConverterJSON::getSettings`:
   - `max_response`: non-negative integer; 0 возвращает 0 результатов (допустимое
     операционное значение, не "без ограничений").
   - `ind_time`: integer >= 1.
   - `max_parallel_readers`: non-negative integer (0 = без ограничения, runtime default).
   - `compact_threshold_percent`: number 0..100.
   - `sqlite_mirror_flush_interval_sec`: number > 0 (принимает float).
   - `sqlite_mirror_max_pending_ops`: non-negative integer; 0 = flush только по таймеру
     (задокументированное операционное значение).
   - `sqlite_load_threads`: integer >= 1.
   - `sqlite_precount_postings`: boolean.
   - `exact_search`, `hide_console_window`: boolean если присутствует.
     Retired BLOCK-1 names (`hide_mode`, `text_request`,
     `save_dictionary_to_file`, `config.Version`, `config.dir`, `config.Name`,
     top-level `Files`) больше не являются active canonical Settings: runtime
     их игнорирует, а SearchEngineConfig снимает/мигрирует при configure.
   - `config.index_roots`, `config.extensions`: non-empty array of strings
     (проверяются элементы; `index_roots` — абсолютные Windows-пути, local
     или UNC). Direct runtime read понимает legacy alias `config.dirs`.
     Persistent canonical migration — обязанность explicit configure/update.
   - `config.excluded_subtrees`: array of non-empty absolute Windows paths
     если присутствует. Direct runtime read понимает legacy alias
     `config.exclude_dirs`.
   - Port precedence: `port` предпочтительнее `asio_port` (соответствует runtime).

7. **`inspect-installed` (скорректирован)**:
   - Проверяет instanceId через `isValidInstanceId()` до обращения к SCM.
   - Использует `utf8()` для wide paths вместо lossy `std::string(begin, end)`.
   - Разрешает relative `--data-dir` относительно каталога EXE (соответствует runtime).
   - Ошибки выводятся через utf8() без потери символов.

8. **`Configure-SearchEngineService.bat`** — portable entrypoint (скорректирован):
   - **Instance picker**: без аргумента вызывает `choose-installed-instance --purpose configure`;
     парсит ответ `instance=...` (не bare значение). HELPER определяется до picker.
   - **Graceful stop**: STOP_TIMEOUT_SECONDS=1800 (соответствует Stop-SearchEngineService).
   - **Rollback STOPPED invariant**: ни один байт Settings.json не восстанавливается без
     явного подтверждения SCM state = STOPPED. State-aware loop обрабатывает
     RUNNING/START_PENDING/STOP_PENDING корректно (stop best-effort при необходимости).
   - **Rollback exit-code gate**: `settings-transaction-rollback` проверяется. При failure —
     rollback-dir сохраняется, commit не вызывается, workflow завершается ошибкой.
   - **Commit только после подтверждённого здоровья старого конфига**: последовательность
     `files restored → firewall restored → old config RUNNING → PING/PONG OLD → commit`.
   - **Firewall dual-naming**:
     - Portable rule (`SearchEngineService[-instance] TCP`): только обновляется localport.
     - PowerShell-installer rule (`<display name> (<port>/TCP)`): старое правило удаляется,
       создаётся новое с новым именем и портом; rollback симметричен.
   - **Firewall failure breaks apply**: если найденное installer-owned правило не обновлено —
     apply не считается успешным, инициируется rollback.
   - **Firewall restore failure = incomplete rollback**: rollback-dir не commit'тится.
   - **`chcp 65001`** перед вызовами helper для корректной обработки UTF-8 output.
   - Endpoint temp генерируется локально BAT-скриптом, если port/year изменились.
   - Hot reload не реализован намеренно (только Stop→Start).

### Известные ограничения и manual recovery paths
- Cyrillic-path round-trip через `cmd.exe` `for /f`: NOT RUN без integration environment.
  `chcp 65001` + `utf8()` — best-effort. Не верифицировано на Windows 7 cmd.
- Если rollback не завершён (STOPPED не достигнут / rollback command failed / firewall
  restore failed): `rollback-dir` сохраняется в `%TEMP%` для ручного восстановления.
  Скрипт выводит диагностику: service name, current SCM state, data-dir, rollback-dir,
  old/new ports.
- `settings-transaction-commit` failure при успешном новом конфиге: только warning.
  Rollback-dir остаётся; ручная очистка. Здоровый новый конфиг не откатывается.
- Disposable integration test: NOT RUN.

### Scope защиты
- Нет `Modify` на весь `%ProgramData%`; работает с правами Admin через SCM.
- Sentinel-файлы (`inverted_index.sqlite`, `auth_clients.sqlite`, `prefix_map.json`,
  `logs/`, `messages/`) не входят в managed set и не изменяются при apply/rollback.

---

## SVC-013 — legacy state и диагностика service mode

**Приоритет:** P2  
**Статус:** VERIFIED

Реализован retirement legacy message queue без изменения wire-слотов:

```text
GET_MESSAGE = 16
SAVE_MESSAGE_TO = 2781032419 (historical composite marker)
```

- Команды сохранены как historical reserved wire slots и отклоняются.
- Текущий сервер не регистрирует handler для `GET_MESSAGE`.
- `SAVE_MESSAGE_TO` (exact marker и historical composite form) классифицируется
  только для безопасного reject.
- Для authenticated сессии body дренируется bounded-буфером и возвращается
  `InvalidCommand` с диагностикой `legacy message queue command is disabled`.
- Для unauthenticated сессии сохранён действующий `AuthRequired` gate.
- `MessageQueue`, `GetMessageCmd`, `SaveMessageCmd` удалены из production-кода.
- `GET_ATTACHMENTS` отделён от legacy queue и использует отдельный live-контейнер
  `AttachmentPackage` с сохранением бинарной совместимости сериализации.
- `messages\` остаётся historical ProgramData state: runtime его больше не
  использует и автоматически не удаляет.
- Ошибки `SqlLogger` insert дублируются в persistent server log
  (`SqlLogger insert failed: ...`) без сохранения SQL-текста.
- `GET_ATTACHMENTS` cleanup больше не пишет success/missing в console; failure
  пишет persistent diagnostic.

---

## SVC-014 — enum содержит unsupported historical commands

**Приоритет:** P2  
**Статус:** VERIFIED

Исторические wire slots **3..9** сохранены без перенумерации и **не имеют
production handler**. Принятый контракт (Option C: docs/tests, без rename на
server):

```text
3  JSONREGUEST
4  ADDRESOLUTION
5  UPDATE            (≠ START_UPDATE_BASE=14)
6  GETRESOLUTIONS
7  GETRESOLUTION
8  GETDOCS
9  GETDOC
```

**Server:** `isRequestCommand=false` → `trustCommand` отклоняет до auth gate и
до чтения body → `InvalidCommand` (`ERROR_RESPONSE` после negotiate, иначе
`SOMEERROR`) с diagnostic `wire_command=N` → **TCP session закрывается**
(`closeAfterWrite=true`). Никогда не доходит до `CommandNotRegistered`.

**Client:** production SearchClient не отправляет 3..9. Имена: slot 3 =
`JSONREGUEST`; slots 4..9 = `RESERVED_COMMAND_*` (wire IDs совпадают с server).

**SOLOREQUEST=1** остаётся **active** на server (`SoloRequestCmd` в cmdMap) для
legacy/third-party клиентов; текущий SearchClient использует 10/12/13.

**Cleanup:** удалены unreachable `SaveTlgToSendCmd` / `SaveFileDefaultCmd`
(SVC-010 legacy upload classes). Stale `AsioServer/` в client repo помечен
archive-only (не в `SearchClient_asio.cbproj`).

**Tests:** `tests/commands/HistoricalCommandSlotTests.cpp`,
`static_assert` ordinals 3..9 на server и client,
расширен `CommandResultTests` allowlist check.

Не переоткрывать: reject semantics для 3..9 (session close), wire ordinals,
SVC-013/009/010/012 contracts.

---

# Матрица команд — кратко

| Команда | Основная зависимость/решение |
|---|---|
| `NEGOTIATE_PROTOCOL_V1` | файловых зависимостей нет |
| `PING` | core installer readiness; SVC-006 |
| `AUTHENTICATE_V1` | persistent auth state; SVC-005 |
| `USER_REGISTRY` | legacy localhost-admin path |
| `SOLOREQUEST` | index + configured `dirs`; server-only legacy (client uses 10/12/13) |
| `GETSQLJSONANSWEAR` | index + configured `dirs` |
| `START_UPDATE_BASE` | full scan/index update |
| `FILETEXT` | reserved / raw request rejected; SVC-009 |
| `GETBINFILE` | reserved / raw request rejected; SVC-009/012 |
| `JSONREGUEST..GETDOC` | historical slots 3..9; trustCommand reject; SVC-014 |
| `GET_VH_TELEGI_FROM_SQL` | `prm_base_dir` |
| `GET_ISH_TELEGI_FROM_SQL` | `prd_base_dir` |
| `GET_ISH_PDTV` | `prd_base_dir`, strict id/read-only lookup |
| `GET_ISH_PDTV_TEXT` | scoped PDTV confirmation; id/slot/entry_index; server-side path resolution |
| `GET_TELEGA_TEXT` | scoped id/type |
| `GET_VH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_ISH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_OPIS_BASE` | configurable `opis_base_dir`; SVC-011 |
| `LOAD_TLG_TO_SEND` | reserved slot 15; server rejects InvalidCommand; SVC-010 |
| `LOAD_RAZN` | reserved slot 22; server rejects InvalidCommand; SVC-010 |
| `UPLOAD_TLG_TO_SEND_V1` | streaming V1; Settings `tlg_send_root`; SVC-010/011 |
| `UPLOAD_RAZN_V1` | streaming V1; Settings `razn_output_dir`; SVC-010/011 |
| `GET_ATTACHMENTS` | primary only + `prefix_map`, destructive buffer semantics; SVC-003/004 |
| `GET_TELEGA_ATACHMENTS` | scoped AutoPad attachment list; SVC-009/012 |
| `GET_SINGLE_ATACHMENT` / `GET_TELEGA_SINGLE_ATACHMENT` | slot 26; naming differs (server/client), wire ordinal один; scoped streaming attachment flow; SVC-009/012 |
| `SAVE_MESSAGE_TO` | historical reserved composite marker; rejected InvalidCommand |
| `GET_MESSAGE` | historical reserved slot 16; rejected InvalidCommand |

---

# Финальный статус service/command audit

```text
FINAL CROSS-REPO AUDIT: CLEAN
server baseline: 59c03b25799da0435edda5ecafd9b3f764a9cfa3
client baseline: 6c2167a2fb885f921c89b40f23e687ab0fef8a46
audit date:      2026-08-19
```

Все зарегистрированные SVC-001..014 имеют финальный статус VERIFIED или
REJECTED. Новых OPEN service/protocol issues на проверенных baseline нет.

Это закрывает данный service/command audit, а не утверждает, что проект не
может иметь будущих bugs/features вне этой области. Не переоткрывать
VERIFIED/REJECTED без нового material fact.

---

# Правила реализации

- Перед кодом соответствующий пункт должен быть `DECIDED`.
- Один пункт или тесно связанная группа — отдельная feature branch/commit.
- Wire command/error/payload изменения проверять согласованно в сервере и клиенте.
- Существующие command/error/payload ordinals не перенумеровывать.
- Installer tests выполнять только на disposable instance и тестовых данных.
- Filesystem tests выполнять во временных каталогах.
- Не запускать destructive scan/update против production data во время тестов.
- Не считать непроведённый build/test успешным: писать `NOT RUN`.

---

# Итоговый отчёт по реализованному пункту

## SVC-010 Part C — Final Reject of Legacy Upload Commands

```text
Issue ID:              SVC-010 (Part C)
Final status:          VERIFIED
Decision:              Legacy commands LOAD_TLG_TO_SEND=15 and LOAD_RAZN=22 are
                       permanently rejected at the server. Wire ordinals preserved.
Server branch/commit:  fix/svc-005-preserve-programdata
                       (Part A: e7c2d6abead99209521a99f3975a8cd9affca32a)
                       (Part C: this commit — see git log)
Client branch/commit:  main / 28af3bd892db0396597c86ad9ad1dddecb88a962
                       (already migrated to 34/35 in Part B)
Changed files (server):
  src/AsioServer/AsioServer.cpp
    — removed cmdMap[COMMAND::LOAD_TLG_TO_SEND] registration
    — removed cmdMap[COMMAND::LOAD_RAZN] registration
    — added reject block: InvalidCommand + "legacy upload is disabled; use streaming V1"
    — removed dead binary-payload log-mask branch for the two legacy commands
  tests/commands/StreamingUploadTests.cpp
    — StreamingUploadContract: cmdMap presence checks flipped from EXPECT_NE to EXPECT_EQ
    — StreamingUploadContract: added sentinel presence check for reject diagnostic string
  docs/HANDOFF_SEARCHENGINE_SERVICE_COMMAND_AUDIT.md
    — SVC-010 status updated DECIDED → VERIFIED
Compatibility impact:  Any client still sending LOAD_TLG_TO_SEND=15 or LOAD_RAZN=22
                       receives typed InvalidCommand (after NEGOTIATE_PROTOCOL_V1)
                       or legacy SOMEERROR (pre-negotiate). Production SearchClient
                       uses 34/35 exclusively since Part B.
Migration/update impact: None for current production client. Old clients will see an
                         error response and must upgrade to streaming V1.
Tests/build/smoke:
  x64 Debug SearchEngine — build: OK
  SearchEngine_test — 116 passed, 3 skipped (symlink); StreamingUploadContract: PASS
Remaining limitations:  Legacy upload command classes removed in SVC-014;
                        `tlg_send_root`/`razn_output_dir` still stored in setSearchServer
                        paths struct for future use by SVC-011.
```

---

```text
Issue ID:
Final status:
Decision:
Server branch/commit:
Client branch/commit (если затронут):
Changed files:
Compatibility impact:
Migration/update impact:
Tests/build/smoke:
Remaining limitations:
```

Не удалять REJECTED/CANCELLED пункты: сохранять причину, чтобы тот же вопрос не
поднимался повторно без новых вводных.
