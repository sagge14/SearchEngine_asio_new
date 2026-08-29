# Логирование в проекте

## Текущее состояние (после унификации)

### 1. Файловые логи — единый механизм (папка `logs/`)

Все файловые логи пишутся в каталог **`logs/`** (создаётся при первом обращении к логеру). Используется один потокобезопасный класс **LogFile** (`MyUtils/LogFile.h`, `LogFile.cpp`).

| Файл в logs/ | Доступ | Назначение |
|--------------|--------|------------|
| `startup.log` | `LogFile::getStartup()` | Старт приложения, LG(...) |
| `watcher.log` | `LogFile::getWatcher()` | FileWatcher, MultiWatcher, Dispatcher, Add/RemoveFileCommand |
| `index.log` | `LogFile::getIndex()` | Индексация (addToLog, logIndexingDebug), очищается при старте |
| `errors.log` | `LogFile::getErrors()` | Ошибки индекса (logIndexError), FATAL/SEH (crash) |
| `backup.log` | `LogFile::getBackup()` | BackupTask: операции и ошибки бэкапа |
| `scan.log` | `LogFile::getScan()` | FileScanner: ошибки сканирования |
| `record.log` | `LogFile::getRecord()` | RecordProcessor (kr_debug) |
| `ping.log` | `LogFile::getPing()` | rawLog, отладка ping |

- **Потокобезопасность:** у каждого лога свой экземпляр LogFile с mutex.
- **Формат:** строка с временной меткой `[%F %T.ms]`, затем сообщение. Для `write(std::wstring)` выполняется преобразование в UTF-8.
- **Макрос LG(...)** по-прежнему используется в main/ContextRuntime и раскрывается в `LogFile::getStartup().write(__VA_ARGS__)`.

### 2. SqlLogger — без изменений

- **SqlLogger** (`MyUtils/SqlLogger.h/.cpp`) по-прежнему пишет только в БД (`log.db`). Не затрагивается унификацией файловых логов.

### 3. Что осталось в консоли (по желанию мигрировать позже)

- **main.cpp**: интерактивное меню, «Search Engine running», «Bye».
- **SearchServer.cpp**: запросы, информация о сервере, отладочные cout.
- **IndexCommands, UpdateOpisBaseCommand, RecordProcessor**: часть wcout/wcerr (исключения, ZAG, опис).
- **ConverterJSON, Encoding, Telega**: разовые cerr/cout.

При необходимости эти выводы можно постепенно перевести в LogFile (например, в `logs/app.log` или по категориям).

### 4. Использование

```cpp
#include "MyUtils/LogFile.h"

// Стартовые сообщения (как раньше LG)
LG("Settings loaded");

// Вотчер, индекс, ошибки, бэкап, скан, запись, пинг
LogFile::getWatcher().write(L"path=" + path);
LogFile::getIndex().write("message");
LogFile::getErrors().write("error line");
LogFile::getBackup().write("Backup dir: ...");
LogFile::getScan().write(utf8_path + " | " + msg);
LogFile::getRecord().write(msg);
LogFile::getPing().write(msg);

// Очистка index.log при старте (опционально)
LogFile::getIndex().clear();
```

Папку `logs/` можно добавить в `.gitignore`, чтобы не коммитить содержимое логов.
