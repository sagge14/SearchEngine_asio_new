# SearchEngine — HANDOFF: аудит команд и работы в режиме Windows-службы

## Назначение

Этот handoff — рабочий реестр проблем, решений и сознательно принятых ограничений,
обнаруженных при проверке перехода `SearchEngine.exe` от ручного запуска к
основному режиму Windows-службы.

Пункты разбираются по одному. Не исправлять всё одним большим рефакторингом.
Для каждого пункта сначала фиксируется решение, затем при необходимости делается
узкая реализация и отдельная проверка.

---

## Контекст аудита

Сервер:

```text
repository: sagge14/SearchEngine_asio_new
branch: main
server audit HEAD: 07c7fa49b6e09b0fc4259ea590ac3635232d61ee
```

Клиент, использованный для проверки реальных вызовов:

```text
repository: myLitleWork/SearchEngine-client
branch: main
client audit HEAD: 07b16d1f6d578544ce842693ddddf35514dbf424
```

Проверялись service lifecycle, runtime paths, installer/update, command dispatch,
файловые команды, AutoPad PRM/PRD, приложения, watcher/full scan, auth и legacy
message/attachment paths.

После существенных изменений этих областей выводы нужно перепроверить по
актуальному HEAD.

---

## Статусы

```text
OPEN        — проблема зафиксирована, решение не принято
ANALYSIS    — идёт отдельный разбор
DECIDED     — решение принято, реализация ещё не обязана быть выполнена
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
| SVC-002 | P0/P1 | Service account и доступ к рабочим путям | OPEN |
| SVC-003 | P0 | `prefix_map.json` для `GET_ATTACHMENTS` при установке | DECIDED |
| SVC-004 | P1/P2 | Семантика `GET_ATTACHMENTS` | DECIDED |
| SVC-005 | P0 | Update/reinstall теряет runtime-state | DECIDED |
| SVC-006 | P0/P1 | `PING/PONG` как единственный health-check | OPEN |
| SVC-007 | P0 | Отсутствующий root воспринимается как отсутствие файлов | REJECTED |
| SVC-008 | P1 | Watcher может не подхватить поздно появившуюся папку | OPEN |
| SVC-009 | P0 security | `GETBINFILE`/`FILETEXT` читают слишком широкий набор путей | OPEN |
| SVC-010 | P0 security | Upload path escape | OPEN |
| SVC-011 | P1 | Жёсткие пути `D:\...` | OPEN |
| SVC-012 | P1 | Основной download приложений через `GETBINFILE` | OPEN |
| SVC-013 | P2 | Legacy state и невидимые `cout/cerr` в service mode | OPEN |
| SVC-014 | P2 | Enum команд шире реально поддерживаемого registry | OPEN |

---

# Принятые решения

## SVC-005 — обычный update не должен заменять `%ProgramData%`

**Статус:** DECIDED  
**Реализация:** ещё не выполнена

### Текущее проблемное поведение

При reinstall/update установщик сейчас отодвигает в rollback не только
`INSTALL_ROOT`, но и весь `%ProgramData%\<service>`, создаёт новый data-dir,
копирует туда свежий package `data\*`, запускает службу и после успешного
`PING/PONG` удаляет старый data-dir.

Из-за этого могут пропасть:

```text
auth_clients.sqlite
issuer-public.pem
inverted_index.sqlite
messages\
prefix_map.json
ignore.txt
log.db
server_log.log
logs\
другие runtime-файлы экземпляра
```

Особенно критична `auth_clients.sqlite`: новая пустая БД может быть технически
валидной, сервер запустится и ответит на `PING`, но зарегистрированные USB/PC
клиенты исчезнут.

### Принятое правило

```text
Program Files = приложение, его можно заменять при update.
ProgramData   = постоянное состояние конкретного экземпляра службы.
```

При обычном update/reinstall того же instance:

- `%ProgramData%\SearchEngineService[-instance]` остаётся на месте;
- `auth_clients.sqlite` сохраняется;
- `issuer-public.pem` сохраняется, если используется sibling key;
- `inverted_index.sqlite` сохраняется;
- `messages\` сохраняется;
- `prefix_map.json` сохраняется;
- пользовательский `ignore.txt` сохраняется;
- `log.db`, `server_log.log`, `logs\` сохраняются;
- неизвестные дополнительные runtime-файлы оператора не удаляются автоматически.

### Что установщик может обновлять

`Settings.json` — особый случай. На update сохраняется уже существующая схема:

```text
новый template Settings.json
        +
старые пользовательские значения
        ->
новый Settings.json
```

То есть использовать существующий `configure-interactive --template ...
--import-settings ...`.

Допустимо пересоздавать `client-endpoint.txt`.

`OEM866.INI` может обновляться из новой поставки как поставляемый runtime-resource,
если это не уничтожает пользовательскую конфигурацию.

### `ignore.txt`

Не перезаписывать пользовательский файл дефолтным при update:

```text
если уже существует -> оставить;
если отсутствует     -> можно скопировать дефолтный из package.
```

### Rollback

Rollback должен в первую очередь откатывать приложение в `Program Files` и те
небольшие конфигурационные файлы, которые реально изменил installer.

Не требуется копировать весь потенциально большой индекс перед каждым update.
Если будущая версия введёт несовместимую миграцию индекса, её rollback/rebuild
должен решаться отдельным изменением.

Критичные маленькие auth/config-файлы можно дополнительно страховать локальной
rollback-копией, но это не должно означать перенос всего data-dir.

Полное удаление `ProgramData`, auth, index, messages и logs выполняется только
через явный full uninstall/reset с предупреждением пользователя.

### Критерий будущей реализации

После update того же instance должны сохраниться auth-клиенты, индекс, сообщения,
пользовательские конфиги и логи. При неудачном запуске новой версии должна быть
возможность вернуть старые binaries без уничтожения persistent state.

---

## SVC-003 — `prefix_map.json` входит в release и настраивается установщиком

**Статус:** DECIDED  
**Реализация:** ещё не выполнена

### Назначение

`GET_ATTACHMENTS` остаётся рабочей функцией. Команда читает относительный
`prefix_map.json`, поэтому в service mode рабочий файл должен оказаться в:

```text
%ProgramData%\SearchEngineService[-instance]\prefix_map.json
```

### Принятое поведение release/package

В релизный комплект SearchEngine входит шаблонный:

```text
data\prefix_map.json
```

Он является заготовкой для настройки конкретного сервера.

### Принятое поведение installer

На этапе установки спрашивать, используется ли на этом сервере функция
`GET_ATTACHMENTS` / клиентская кнопка «Сохранить приложения».

Если пользователь отвечает **нет**:

- `prefix_map.json` не является обязательным;
- его отсутствие не блокирует установку;
- функция считается неиспользуемой на данном экземпляре.

Если пользователь отвечает **да**:

- проверить наличие `data\prefix_map.json` в установочном комплекте;
- если файл отсутствует — вывести явное предупреждение, что
  `GET_ATTACHMENTS` работать не будет;
- отсутствие файла само по себе не обязано блокировать установку;
- если файл есть — проверить хотя бы корректность JSON и ожидаемую структуру
  `prefix` + `map`;
- после проверки скопировать файл в активный data-dir экземпляра.

При обычном update существующий рабочий `%ProgramData%\...\prefix_map.json`
сохраняется по решению SVC-005 и не должен молча заменяться шаблоном из новой
поставки.

---

## SVC-004 — текущая семантика `GET_ATTACHMENTS` сохраняется

**Статус:** DECIDED  
**Реализация:** изменение бизнес-логики не требуется

Принято оставить существующий сценарий:

1. клиент вызывает `GET_ATTACHMENTS` только на `primaryServerId()`;
2. сервер по имени оператора и `prefix_map.json` находит буферный каталог;
3. рекурсивно читает его файлы;
4. после успешного чтения удаляет исходный буферный каталог;
5. возвращает содержимое клиенту.

Не вводить сейчас ACK, quarantine, processed-folder или опрос дополнительных
серверов.

Известный риск потери буфера при ошибке после чтения/удаления принят как часть
текущей legacy-семантики. Если в будущем потребуется гарантированная доставка,
это будет отдельная задача.

---

## SVC-007 — текущее поведение отсутствующих месячных папок принято

**Статус:** REJECTED  
**Исправление:** не требуется

### Реальная бизнес-модель

В `Settings.json` нормально заранее иметь все месячные каталоги:

```text
D:\ЯНВАРЬ
D:\ФЕВРАЛЬ
D:\МАРТ
...
D:\ДЕКАБРЬ
```

При этом физически могут существовать только месяцы, которые уже наступили.
Например в январе нормально, если есть только `D:\ЯНВАРЬ`.

### Принятое компромиссное поведение

Сейчас не вводить сложную модель состояний root и не различать специально:

- физическое удаление каталога;
- временную недоступность диска;
- сетевой/mapped-drive сценарий;
- Access Denied;
- позднее появление тома;
- иные причины отсутствия файлов.

Если настроенная папка физически отсутствует и полный scan видит её как пустую,
старые документы этого каталога могут быть помечены `deleted`.

Если папка затем появляется снова, очередной scan индексирует её файлы обратно.

Это поведение принято сознательно как простой текущий контракт. SVC-007 не надо
«улучшать» без нового отдельного решения.

### Будущая идея — не часть SVC-007

В будущем отдельно рассмотреть режим **freeze / заморозки индекса**:

```text
сервер продолжает поиск по текущему индексу;
full scan не изменяет индекс;
watcher add/remove/change не изменяет индекс;
состояние фиксируется до явного снятия freeze.
```

Это отдельная будущая feature, а не исправление текущего scanner.

---

# Открытые пункты

## SVC-001 — runtime-root службы и управление настройками

**Приоритет:** P1  
**Статус:** OPEN

Service mode использует `%ProgramData%\SearchEngineService[-instance]` как
`--data-dir` и CWD. Поэтому `Settings.json`, индекс, auth DB, logs/messages и
другие относительные runtime-файлы находятся там, а не рядом с EXE.

Проблема эксплуатационная: оператор, привыкший редактировать файлы рядом с EXE,
может изменить неиспользуемую копию. Нельзя ради удобства давать обычным
пользователям Modify на весь data-dir, потому что там auth DB и индекс.

Варианты на будущее: документированный stop/edit/validate/start, admin helper,
разделение config/state/logs или безопасный reload отдельных настроек.

---

## SVC-002 — service account и доступ к рабочим путям

**Приоритет:** P0/P1  
**Статус:** OPEN

Установщик сейчас не задаёт `obj=`, поэтому служба работает как `LocalSystem`.
Локальный физический `D:` обычно доступен, если ACL позволяют. Пользовательские
mapped drives, `subst`, profile credentials и часть сетевых ресурсов могут быть
не видны service context.

Затронуты index roots, AutoPad PRM/PRD, F12/OPIS, attachments, raw downloads и
upload destinations.

Решение пока не принято. Не смешивать с SVC-007: там сознательно принято текущее
поведение scanner, здесь вопрос именно прав и поддерживаемой service account.

---

## SVC-006 — `PING/PONG` недостаточен как readiness-check

**Приоритет:** P0/P1  
**Статус:** OPEN

Installer считает новую установку здоровой, если SCM показывает `RUNNING` и
сервер отвечает на `PING`.

`PING` проверяет liveness, но не подтверждает:

- сохранность/работоспособность auth state;
- доступность index;
- наличие optional `prefix_map.json`;
- рабочие PRM/PRD/F12/OPIS sources;
- права чтения/записи для command paths.

После решения SVC-005 часть риска уменьшается, но вопрос readiness остаётся.
Варианты: отдельный installer diagnostic/smoke-check или структурированный
readiness endpoint. Optional функции не должны блокировать установку, если они
явно не используются.

---

## SVC-008 — watcher может не подхватить поздно появившийся child/root

**Приоритет:** P1  
**Статус:** OPEN

`MultiDirWatcher` создаёт inner watcher для уже существующих configured kids.
Если parent/root отсутствовал при старте, после его позднего появления watcher
может начать ждать новые directory events, не выполнив повторное перечисление
уже существующих configured child directories.

Следствие: периодический full scan позже увидит файлы, но realtime watcher может
не следить за каталогом до нового directory event или перезапуска.

Возможные решения:

- при каждом успешном open/reopen parent повторно проверять существующие kids;
- периодически сверять configured kids и active inner watchers;
- после появления child создавать watcher немедленно.

Нужно разобрать с учётом месячной схемы: папки `ЯНВАРЬ...ДЕКАБРЬ` нормально
появляются по ходу года.

---

## SVC-009 — `GETBINFILE`/`FILETEXT` читают произвольный доступный путь

**Приоритет:** P0 security  
**Статус:** OPEN

Авторизованный клиент может передать серверный путь. Проверки в основном
ограничены существованием/open/read, без строгой canonical allowlist рабочих
roots.

Под `LocalSystem` это даёт endpoint более широкие права, чем требуются поисковому
клиенту.

Варианты: canonical allowlist, server-side IDs вместо raw paths, scoped
attachment endpoint, отдельные capabilities, ограниченная service account.

Не менять wire ordinals без двухрепозиторного плана.

---

## SVC-010 — upload-команды могут выйти из base path

**Приоритет:** P0 security  
**Статус:** OPEN

`LOAD_TLG_TO_SEND` и `LOAD_RAZN` используют полученное имя/подпуть для
формирования destination. Требуется отдельно решить защиту от `..`, absolute,
UNC/rooted paths и reparse escape.

Варианты: разрешать только basename либо безопасный relative path с canonical
containment check.

Отдельно решить старую duplicate-логику, где совпадение размера может считаться
достаточным для совпадения файла.

---

## SVC-011 — жёстко заданные production paths

**Приоритет:** P1  
**Статус:** OPEN

Подтверждённые примеры:

```text
LOAD_TLG_TO_SEND -> D:\ + <МЕСЯЦ> + <дата>
LOAD_RAZN        -> D:\OPIS_ADMIN\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ\
GET_OPIS_BASE    -> D:\OPIS_ADMIN\<year>.db
GET_*_TELEGA_WAY -> D:\F12\<year>.db и D:\F12\base.db
RecordProcessor  -> D:\OPIS_ADMIN\<year>.DB
```

PRM/PRD уже частично конфигурируются через Settings, но не все filesystem roots.
Нужно отдельно решить, какие пути оставить исторически фиксированными, а какие
вынести в Settings.

---

## SVC-012 — основной client attachment download через `GETBINFILE`

**Приоритет:** P1  
**Статус:** OPEN

Команды `GET_PRIL` в wire-контракте нет.

При обычном сохранении найденных документов SearchClient строит абсолютный путь
к приложению/zip и скачивает через `GETBINFILE`. Поэтому смена CWD службы сама по
себе этот путь не ломает.

На сервере уже есть более scoped:

```text
GET_TELEGA_ATACHMENTS
GET_SINGLE_ATACHMENT
```

Они получают id/type и разрешают attachment на стороне сервера. Решение о
переходе основного UI на них пока не принято. Это будет двухрепозиторное
изменение.

---

## SVC-013 — legacy state и диагностика service mode

**Приоритет:** P2  
**Статус:** OPEN

`MessageQueue` использует `current_path()/messages`; благодаря service CWD это
сейчас попадает в data-dir. Legacy `GET_MESSAGE` всегда использует `user_id=1`.
`UserRegistry/users.dat` не является основным текущим auth-путём.

Часть legacy-ошибок всё ещё выводится только через `cout/cerr`, которые оператор
службы не видит. Нужно позже решить судьбу endpoints и перевести важные ошибки в
structured logs/typed responses.

---

## SVC-014 — enum содержит исторические unsupported команды

**Приоритет:** P2  
**Статус:** OPEN

В списке имён остаются команды без активного request handler, включая:

```text
JSONREGUEST
ADDRESOLUTION
UPDATE
GETRESOLUTIONS
GETRESOLUTION
GETDOCS
GETDOC
```

Нельзя просто удалить элементы из середины последовательного wire enum и тем
самым перенумеровать существующие команды.

Варианты: explicit ordinals + reserved/unsupported documentation и cross-repo
compile/tests.

---

# Матрица команд — кратко

| Команда | Основная зависимость/риск |
|---|---|
| `NEGOTIATE_PROTOCOL_V1` | файловых зависимостей нет |
| `PING` | только liveness; SVC-006 |
| `AUTHENTICATE_V1` | persistent auth state; SVC-005/006 |
| `USER_REGISTRY` | legacy localhost-admin path |
| `SOLOREQUEST` | индекс и scanner roots |
| `GETSQLJSONANSWEAR` | индекс и scanner roots |
| `START_UPDATE_BASE` | full scan/index update |
| `FILETEXT` | raw path read; SVC-009 |
| `GETBINFILE` | raw path read; SVC-009/012 |
| `GET_VH_TELEGI_FROM_SQL` | PRM source |
| `GET_ISH_TELEGI_FROM_SQL` | PRD source |
| `GET_ISH_PDTV` | PRD source |
| `GET_VH_TELEGA_WAY` | `D:\F12`; SVC-011 |
| `GET_ISH_TELEGA_WAY` | `D:\F12`; SVC-011 |
| `GET_OPIS_BASE` | `D:\OPIS_ADMIN`; SVC-011 |
| `LOAD_TLG_TO_SEND` | hardcoded root + upload path; SVC-010/011 |
| `LOAD_RAZN` | hardcoded OPIS path + upload path; SVC-010/011 |
| `GET_ATTACHMENTS` | primary only, `prefix_map`, buffer delete; SVC-003/004 |
| `GET_TELEGA_ATACHMENTS` | scoped AutoPad attachment list |
| `GET_SINGLE_ATACHMENT` | scoped attachment read |
| `SAVE_MESSAGE_TO` | legacy messages state |
| `GET_MESSAGE` | legacy user 1 queue |

`SOMEERROR`, `SERVER_BUSY_ERROR`, `ERROR_RESPONSE` — response/error values;
`END_COMMAND` — sentinel.

---

# Что уже подтверждено и не считается отдельной поломкой

1. Console и service mode создают один `SearchEngineApplication` и один набор
   command handlers; отдельной урезанной service-версии команд нет.
2. Основной attachment download через `GETBINFILE` передаёт абсолютный путь;
   смена CWD сама его не ломает.
3. `GET_TELEGA_ATACHMENTS` и `GET_SINGLE_ATACHMENT` получают рабочие пути через
   AutoPad данные на сервере.
4. `MessageQueue` сейчас попадает в active data-dir из-за явной установки CWD.
5. `GET_PRIL` как wire-команды нет.
6. Отсутствующие будущие месячные папки в Settings — нормальная эксплуатационная
   модель; по SVC-007 текущее scanner-поведение принято сознательно.

---

# Текущий порядок дальнейшего разбора

Уже разобраны:

```text
SVC-005 -> DECIDED
SVC-003 -> DECIDED
SVC-004 -> DECIDED
SVC-007 -> REJECTED
```

Дальше:

1. **SVC-008** — корректно ли watcher подхватывает месячную папку, появившуюся
   после запуска службы.
2. **SVC-009 + SVC-010** — filesystem security boundary.
3. **SVC-002 + SVC-011** — service account и hardcoded roots.
4. **SVC-006** — readiness/health после определения обязательных функций.
5. **SVC-012** — нужен ли переход клиента на scoped attachment endpoints.
6. **SVC-001, SVC-013, SVC-014** — эксплуатационный UX и legacy cleanup.

Порядок можно менять отдельным решением.

---

# Правила реализации

- Перед кодом соответствующий пункт должен быть `DECIDED` с понятным контрактом.
- Один пункт или тесно связанная группа — отдельная feature branch/commit.
- Не смешивать installer persistence, security и protocol redesign без причины.
- Wire command/error/payload изменения проверять в сервере и клиенте.
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

Не удалять отвергнутые/отменённые пункты: сохранять `REJECTED`/`CANCELLED` и
причину, чтобы тот же вопрос не поднимался повторно без новых вводных.
