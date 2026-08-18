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
| SVC-001 | P1 | Runtime-root и редактирование настроек | OPEN |
| SVC-002 | P0/P1 | Service account и доступ к рабочим путям | DECIDED |
| SVC-003 | P0 | `prefix_map.json` для `GET_ATTACHMENTS` при установке | DECIDED |
| SVC-004 | P1/P2 | Семантика `GET_ATTACHMENTS` | DECIDED |
| SVC-005 | P0 | Update/reinstall теряет runtime-state | DECIDED |
| SVC-006 | P0/P1 | `PING/PONG` как core health-check | REJECTED |
| SVC-007 | P0 | Отсутствующий root воспринимается как отсутствие файлов | REJECTED |
| SVC-008 | P1 | Watcher может не подхватить поздно появившийся parent/root | OPEN |
| SVC-009 | P0 security | `GETBINFILE`/`FILETEXT` принимают raw server paths | DECIDED |
| SVC-010 | P0 security | Upload path escape и upload целиком в RAM | DECIDED |
| SVC-011 | P1 | Жёсткие production paths `D:\...` | DECIDED |
| SVC-012 | P1 | Основной download приложений через `GETBINFILE` | DECIDED |
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

## SVC-009 + SVC-012 — scoped-доступ к тексту и приложениям, end-to-end streaming

**Статус:** DECIDED

Все загрузки приложений переводятся на безопасную схему:

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

**Статус:** DECIDED

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
**Статус:** OPEN

Service mode использует `%ProgramData%\SearchEngineService[-instance]` как
`--data-dir` и CWD. Оператор может по привычке редактировать копию Settings рядом
с EXE, которая не используется.

Нужно позже решить UX безопасного редактирования/валидации/reload config без
выдачи Modify на весь data-dir, где лежат auth DB и индекс.

---

## SVC-008 — watcher и позднее появление parent/root

**Приоритет:** P1  
**Статус:** OPEN

Нормальный сценарий, когда parent (например `D:\`) существует, а configured
месячная папка появляется позже, уже подхватывается parent watcher: создаётся
inner `FileWatcher` с `forceWalk=true`, который рекурсивно выдаёт Added для уже
находящихся там файлов.

Остаётся только edge case: сам parent/root отсутствовал при старте и позднее
появился уже с существующими configured children. После открытия parent текущий
код не делает явного повторного enumerate kids и может ждать будущего directory
event.

С учётом решения SVC-007 предварительная рекомендация — не усложнять этот edge
case сейчас и перевести SVC-008 в REJECTED отдельным решением.

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
| `FILETEXT` | после scoped migration отклонять raw path; SVC-009 |
| `GETBINFILE` | после scoped migration отклонять raw path; SVC-009/012 |
| `GET_VH_TELEGI_FROM_SQL` | `prm_base_dir` |
| `GET_ISH_TELEGI_FROM_SQL` | `prd_base_dir` |
| `GET_ISH_PDTV` | `prd_base_dir` |
| `GET_VH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_ISH_TELEGA_WAY` | configurable `f12_base_dir`; SVC-011 |
| `GET_OPIS_BASE` | configurable `opis_base_dir`; SVC-011 |
| `LOAD_TLG_TO_SEND` | configurable `tlg_send_root`, safe streaming upload; SVC-010/011 |
| `LOAD_RAZN` | configurable `razn_output_dir`, safe streaming upload; SVC-010/011 |
| `GET_ATTACHMENTS` | primary only + `prefix_map`, destructive buffer semantics; SVC-003/004 |
| `GET_TELEGA_ATACHMENTS` | scoped AutoPad attachment list; SVC-009/012 |
| `GET_SINGLE_ATACHMENT` | scoped streaming attachment read; SVC-009/012 |
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
SVC-009 -> DECIDED
SVC-010 -> DECIDED
SVC-012 -> DECIDED
SVC-002 -> DECIDED
SVC-011 -> DECIDED
SVC-006 -> REJECTED
```

Дальше:

1. **SVC-008** — формально закрыть watcher edge-case, если решение принято.
2. **SVC-001** — runtime config UX.
3. **SVC-013 + SVC-014** — legacy cleanup/diagnostics/wire slots.

---

# Правила реализации

- Перед кодом соответствующий пункт должен быть `DECIDED`.
- Один пункт или тесно связанная группа — отдельная feature branch/commit.
- Wire command/error/payload изменения проверять согласованно в сервере и клиенте.
- Существующие command/error ordinals не перенумеровывать.
- Installer tests выполнять только на disposable instance и тестовых данных.
- Filesystem tests выполнять во временных каталогах.
- Не запускать destructive scan/update против production data во время тестов.
- Не считать непроведённый build/test успешным: писать `NOT RUN`.

---

# Итоговый отчёт по реализованному пункту

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
