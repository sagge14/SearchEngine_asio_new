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
server audit baseline: 07c7fa49b6e09b0fc4259ea590ac3635232d61ee

client: myLitleWork/SearchEngine-client
branch: main
client audit baseline: 07b16d1f6d578544ce842693ddddf35514dbf424
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
| SVC-001 | P1 | Runtime-root и редактирование настроек | FIXED |
| SVC-002 | P0/P1 | Service account и доступ к рабочим путям | DECIDED |
| SVC-003 | P0 | `prefix_map.json` для `GET_ATTACHMENTS` при установке | DECIDED |
| SVC-004 | P1/P2 | Семантика `GET_ATTACHMENTS` | DECIDED |
| SVC-005 | P0 | Update/reinstall теряет runtime-state | DECIDED |
| SVC-006 | P0/P1 | `PING/PONG` как core health-check | REJECTED |
| SVC-007 | P0 | Отсутствующий root воспринимается как отсутствие файлов | REJECTED |
| SVC-008 | P1 | Watcher может не подхватить поздно появившийся parent/root | REJECTED |
| SVC-009 | P0 security | `GETBINFILE`/`FILETEXT` принимают raw server paths | VERIFIED |
| SVC-010 | P0 security | Upload path escape и upload целиком в RAM | VERIFIED |
| SVC-011 | P1 | Жёсткие production paths `D:\...` | DECIDED |
| SVC-012 | P1 | Основной download приложений через `GETBINFILE` | VERIFIED |
| SVC-013 | P2 | Legacy state и невидимые `cout/cerr` | OPEN |
| SVC-014 | P2 | Enum команд шире реально поддерживаемого registry | OPEN |

---

# Принятые решения

## SVC-005 — ProgramData является persistent state

**Статус:** DECIDED

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
требуется. Полное удаление ProgramData допускается только при явном full
uninstall/reset с предупреждением.

---

## SVC-003 — `prefix_map.json` входит в release и настраивается installer

**Статус:** DECIDED

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

**Статус:** DECIDED

Оставить текущий сценарий:

1. клиент вызывает `GET_ATTACHMENTS` только на `primaryServerId()`;
2. сервер по оператору и `prefix_map.json` находит буферный каталог;
3. читает его файлы;
4. после успешного чтения удаляет исходный буферный каталог;
5. возвращает содержимое клиенту.

Не вводить сейчас ACK/quarantine/processed-folder. Риск удаления буфера до
окончательной доставки принят как часть текущей legacy-семантики.

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

## SVC-002 — service account: LocalSystem остаётся поддерживаемым режимом

**Статус:** DECIDED

Installer сейчас создаёт/configure службу без `obj=`, поэтому Windows запускает
её под `LocalSystem`. Это поведение оставить.

На текущем этапе официальный эксплуатационный контракт:

```text
SearchEngineService работает как LocalSystem.
Рабочие данные находятся на локальных физических дисках/путях,
доступных этому service account по ACL.
```

Сейчас не добавлять:

- выбор service account в installer;
- логин/пароль службы;
- автоматический `net use`;
- поддержку user mapped drives;
- `subst`/profile-specific drive mappings;
- хранение credentials для UNC/network shares.

Если позже понадобится сетевое хранилище, это отдельная задача по service
account/UNC и credential model.

Installer/config diagnostics должны явно показывать:

```text
Service account: LocalSystem
```

и документация не должна создавать впечатление, что пользовательские mapped
drives автоматически видны службе.

Отсутствие/недоступность будущих monthly roots не является startup-fatal само по
себе — это остаётся решением SVC-007.

---

## SVC-011 — все скрытые production roots выносятся в Settings

**Статус:** DECIDED

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

- `LOAD_TLG_TO_SEND`;
- `LOAD_RAZN`;
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

# Открытые пункты

## SVC-001 — runtime-root службы и управление настройками

**Приоритет:** P1  
**Статус:** FIXED

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

6. **`validateJson()` (расширена)** — дополнительные type/range checks для полей:
   `max_response` (>= 1), `ind_time` (>= 1), `max_parallel_readers` (0..65535,
   0 = no limit), `compact_threshold_percent` (0..100), `sqlite_mirror_flush_interval_sec`
   (number > 0), `sqlite_mirror_max_pending_ops` (>= 1), `sqlite_load_threads` (1..64),
   `sqlite_precount_postings` (boolean).

7. **`Configure-SearchEngineService.bat`** — portable entrypoint:
   - Resolves `data_dir` через `inspect-installed` (не `%ProgramData%`).
   - Копирует `Settings.json` в `%TEMP%` для редактирования.
   - Проходит через `validate` как обязательный gate.
   - Stop → `settings-transaction-apply` → firewall update (если нужно) → Start →
     wait RUNNING → `health --port NEW` → PING/PONG.
   - При failure: `settings-transaction-rollback` → restore firewall → Start на
     старом порту → `health --port OLD`.
   - Endpoint temp генерируется локально BAT-скриптом, если `oldPort != newPort`.
   - Hot reload не реализован намеренно (только Stop→Start).

### Scope защиты
- Нет `Modify` на весь `%ProgramData%`; работает с правами Admin через SCM.
- Sentinel-файлы (`inverted_index.sqlite`, `auth_clients.sqlite`, `prefix_map.json`,
  `logs/`, `messages/`) не входят в managed set и не изменяются при apply/rollback.

---

## SVC-013 — legacy state и диагностика service mode

**Приоритет:** P2  
**Статус:** OPEN

`MessageQueue` использует `current_path()/messages`; service CWD направляет его в
active data-dir. Legacy `GET_MESSAGE` использует `user_id=1`. Часть legacy
ошибок всё ещё идёт только в `cout/cerr`, невидимые оператору службы.

Нужно позже решить судьбу legacy endpoints и перевести важные ошибки в structured
logs/typed responses.

---

## SVC-014 — enum содержит unsupported historical commands

**Приоритет:** P2  
**Статус:** OPEN

Остаются исторические wire slots без активного handler:

```text
JSONREGUEST
ADDRESOLUTION
UPDATE
GETRESOLUTIONS
GETRESOLUTION
GETDOCS
GETDOC
```

Нельзя удалять их из середины последовательного enum с перенумерацией остальных.
Нужно позже закрепить explicit/reserved ordinals и cross-repo tests/docs.

---

# Матрица команд — кратко

| Команда | Основная зависимость/решение |
|---|---|
| `NEGOTIATE_PROTOCOL_V1` | файловых зависимостей нет |
| `PING` | core installer readiness; SVC-006 |
| `AUTHENTICATE_V1` | persistent auth state; SVC-005 |
| `USER_REGISTRY` | legacy localhost-admin path |
| `SOLOREQUEST` | index + configured `dirs` |
| `GETSQLJSONANSWEAR` | index + configured `dirs` |
| `START_UPDATE_BASE` | full scan/index update |
| `FILETEXT` | reserved / raw request rejected; SVC-009 |
| `GETBINFILE` | reserved / raw request rejected; SVC-009/012 |
| `GET_VH_TELEGI_FROM_SQL` | `prm_base_dir` |
| `GET_ISH_TELEGI_FROM_SQL` | `prd_base_dir` |
| `GET_ISH_PDTV` | `prd_base_dir`, strict id/read-only lookup |
| `GET_ISH_PDTV_TEXT` | scoped PDTV confirmation; id/slot/entry_index; server-side path resolution |
| `GET_TELEGA_TEXT` | scoped id/type |
| `GET_VH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_ISH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_OPIS_BASE` | configurable `opis_base_dir`; SVC-011 |
| `LOAD_TLG_TO_SEND` | configurable `tlg_send_root`, safe streaming upload; SVC-010/011 |
| `LOAD_RAZN` | configurable `razn_output_dir`, safe streaming upload; SVC-010/011 |
| `GET_ATTACHMENTS` | primary only + `prefix_map`, destructive buffer semantics; SVC-003/004 |
| `GET_TELEGA_ATACHMENTS` | scoped AutoPad attachment list; SVC-009/012 |
| `GET_SINGLE_ATACHMENT` | scoped attachment flow; SVC-009/012 |
| `SAVE_MESSAGE_TO` | legacy messages state |
| `GET_MESSAGE` | legacy queue/user 1 |

---

# Текущий порядок дальнейшего разбора

Уже разобраны:

```text
SVC-005 -> DECIDED
SVC-003 -> DECIDED
SVC-004 -> DECIDED
SVC-007 -> REJECTED
SVC-008 -> REJECTED
SVC-009 -> VERIFIED
SVC-010 -> VERIFIED
SVC-012 -> VERIFIED
SVC-002 -> DECIDED
SVC-011 -> DECIDED
SVC-006 -> REJECTED
```

Дальше:

1. **SVC-001** — runtime config UX.
2. **SVC-013 + SVC-014** — legacy cleanup/diagnostics/wire slots.

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
Remaining limitations:  SaveTlgToSendCmd and SaveFileDefaultCmd classes remain in
                        source but are unreachable — removal is SVC-011/SVC-013 scope.
                        tlg_send_root/razn_output_dir still stored in setSearchServer
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
