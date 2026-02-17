# Аудит вывода в консоль

## Откуда идёт вывод (по цепочке от вотчера)

Цепочка при событии файла: **FileWatcher → Dispatcher → AddFileCommand** → затем:
- **ZagKpodiChenger** (ZAG-файлы) → `wcout`
- **UpdateOpisBaseCommand** (опис) → много `wcout`/`wcerr`
- **RecordProcessor** (запись в БД) → `wcout`/`cerr`/`wcerr`

То есть сам Watcher/Dispatcher уже пишут в **logs/watcher.log**, но команды, которые они вызывают (AddFileCommand → opis, ZAG, RecordProcessor), по-прежнему выводят в консоль. Поэтому «вывод от вотчера» в консоли всё ещё есть.

---

## Полный список вывода в консоль

### 1. Main (оставить в консоль — UI)
| Где | Что |
|-----|-----|
| main.cpp:98, 100 | `[PING post ->]` / `[PING executed <-]` (отладка ping) |
| main.cpp:303 | `--- Search Engine running ---` + меню |
| main.cpp:324 | `--- Bye ---` |

**Предложение:** меню и «running»/«Bye» оставить. Ping можно перенести в `LogFile::getPing()` (уже есть rawLog → getPing).

---

### 2. FileWatcher (цепочка команд — перенести в лог)
Всё это вызывается при обработке событий от вотчера (AddFileCommand / opis / ZAG).

| Файл | Строки | Что выводится |
|------|--------|----------------|
| **IndexCommands.h** | 59 | `[ZAG] = path` |
| **IndexCommands.h** | 72, 74, 77 | `[execute] opis_command_ exception`, `nullptr`, path |
| **ZagKpodiChenger.h** | 148 | `[key] = value` |
| **UpdateOpisBaseCommand.h** | 24, 28, 32, 41, 71, 89, 96, 106, 111 | `[opis_command]` Skipped / created / ERROR / No update / Podrazd / setAndCheckLists / exceptions |
| **UpdateOpisBaseCommand.h** | 106, 108, 114, 116 | `[opis_command]` Exception (wcerr) |
| **RecordProcessor.cpp** | 53, 61 | `[RecordProcessor]` Constructing / No record |
| **RecordProcessor.cpp** | 82, 90, 97, 113 | `[RecordProcessor]` exceptions (wcerr) |
| **RecordProcessor.cpp** | 158, 163, 172, 183, 185, 246, 254, 259, 281, 286, 293, 351, 356 | `[run]` / `[RecordProcessor]` ошибки, empty kr (cerr/cout) |
| **PodrIgnore.cpp** | 63 | `PodrIgnore:` (cerr) |

**Предложение:** всё из п.2 писать в **LogFile::getWatcher()** (или часть в **getRecord()** для RecordProcessor/opis). Тогда вся трасса «событие файла → команда → опис/ZAG/RecordProcessor» будет в файле, консоль от вотчера пустая.

---

### 3. Индекс (InvertedIndex)
| Файл | Строки | Что |
|------|--------|-----|
| InvertedIndex.cpp | 776, 780, 794, 800, 808, 813, 829, 835, 840, 844, 861, 869, 877, 885, 889, 915, 921, 923 | `[enqueueFileUpdate]`, `[strand/...]`, `[fileIndexing]`, `[enqueueFileDeletion]`, исключения |

**Предложение:** перенести в **LogFile::getIndex()** (уже договорились, что весь вывод индексации — в index.log).

---

### 4. Планировщик (PeriodicTaskManager)
| Файл | Строки | Что |
|------|--------|-----|
| PeriodicTaskManager.h | 35, 46, 60, 64, 68, 76, 78, 86 | `[scheduler_tick]` Not time yet / Condition false / Action executed / Exception / Retry / Queue size |

**Предложение:** перенести в **LogFile::getIndex()** или в отдельный **getScheduler()** (например `logs/scheduler.log`), либо оставить только при отладке (флаг/уровень).

---

### 5. SearchServer
| Файл | Строки | Что |
|------|--------|-----|
| SearchServer.cpp | 45 | `[NEW]` word (временный лог) |
| SearchServer.cpp | 137, 143, 170, 177 | Request start/finish, Update base running |
| SearchServer.cpp | 365 | get index wordT |
| SearchServer.cpp | 428–442 | Server information (блок) |

**Предложение:** отладочные строки (NEW, Request start/finish, wordT) → **getIndex()** или общий **logs/app.log**. Блок «Server information» можно оставить в консоль при старте или вынести в startup.log.

---

### 6. Ошибки и служебные (куда писать)
| Файл | Куда логировать |
|------|------------------|
| SqlLogger.h (LOG FAIL) | Оставить cout или → **LogFile::getErrors()** |
| SqlLogger.cpp (- new, Log DB error) | → **getErrors()** или оставить (если нужен только консольный мониторинг БД) |
| ConverterJSON.h/cpp (Error pars settings, JSON parse error) | **getErrors()** |
| Encoding.cpp (oem866/win1251 errors) | **getErrors()** |
| GetJsonTelega/Telega, GetJsonTelegaCmd, GetIshTelegaPdtv (Error, sql error) | **getErrors()** |
| GetAttachments (директория удалена/не существует, ошибка удаления) | **getWatcher()** или **getErrors()** |
| SaveFile/FileData (Serialization/Deserialization error) | **getErrors()** |
| SaveMessage/MessageQueue (Failed to open, Invalid argument) | **getErrors()** |

**Предложение:** единообразно: все ошибки (исключения, «error», «fail») → **LogFile::getErrors()**; события, связанные с файлами/вотчером, — в **getWatcher()** по смыслу.

---

### 7. Специальные случаи
| Файл | Примечание |
|------|------------|
| robin_hood.h (строки 59, 68) | Под макросом отладки (ROBIN_HOOD_LOG_ENABLED), не трогать. |
| GetIshTelegaPdtvCommand.cpp:44 | `std::cout << jsonString` — похоже на отладочный вывод ответа; при необходимости → лог (например, getErrors или отдельный debug). |

---

## Итоговые рекомендации

1. **Оставить в консоли только:** приветствие «Search Engine running», меню, «Bye», при желании — блок Server information при старте.
2. **Вся цепочка вотчера (п.2)** — в **logs/watcher.log** через **LogFile::getWatcher()**: IndexCommands (ZAG, execute), ZagKpodiChenger, UpdateOpisBaseCommand, RecordProcessor, PodrIgnore. Тогда «вывод от вотчера» в консоли пропадёт.
3. **Индекс (п.3)** — все wcout/wcerr из InvertedIndex → **LogFile::getIndex()**.
4. **Планировщик (п.4)** — либо в **logs/scheduler.log** (добавить getScheduler()), либо в getIndex(), либо за флагом «verbose».
5. **SearchServer (п.5)** — отладочные строки в лог (getIndex или app), Server information — по желанию в консоль или в startup.log.
6. **Ошибки (п.6)** — по возможности в **LogFile::getErrors()**; привязка к файлам/вотчеру — дублировать или только в **getWatcher()** по смыслу.
7. **Ping в main** — перевести на **LogFile::getPing()**, если не нужен в консоли.

Если нужно, могу следующим шагом расписать конкретные правки по файлам (какие строки заменить на вызовы LogFile и с каким логом).
