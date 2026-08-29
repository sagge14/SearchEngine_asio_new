# Руководство агенту: GUI поверх BackupRestoreCore

Документ для агента, который реализует графический интерфейс восстановления
поверх уже готового ядра `BackupRestoreCore`. CLI (`BackupRestore.exe`) —
эталон поведения; GUI не дублирует логику сканирования/хешей/копирования.

Связанные материалы:

- ядро: [`src/Backup/Restore/`](../src/Backup/Restore/)
- CLI-эталон: [`tools/backup_restore/`](../tools/backup_restore/)
- формат хранилища: [`BACKUP_MIRROR_HISTORY.md`](BACKUP_MIRROR_HISTORY.md)
- обзор утилиты: [`BACKUP_RESTORE_UTILITY_PROPOSAL.md`](BACKUP_RESTORE_UTILITY_PROPOSAL.md)
- unit-тесты ядра: [`tests/backup/BackupRestoreTests.cpp`](../tests/backup/BackupRestoreTests.cpp)

---

## 1. Цель продукта

Окно восстановления для `mirror_history` (профиль economical / каталог вида
`F:\AutoPadEconomicalBackups`):

1. Выбрать корень бэкапа.
2. Увидеть **список целей** (`BASES`, `BASES_PRD`, `F12`, …).
3. Увидеть **таблицу точек** (дата, время, tier, complete, число файлов, размер).
4. Увидеть **таблицу файлов** выбранной точки (путь, размер, статус, captured).
5. Выбрать **что** восстанавливать (все или отмеченные файлы) и **куда**.
6. Проверить (`verify`) и восстановить (`restore`) с прогрессом и отменой.

В первой версии GUI: **только `mirror_history`**. Snapshot не поддерживать,
пока не появится вторая фабрика сервисов.

Целевой UI toolchain: **RAD Studio / C++Builder (VCL, Win32 в приоритете)**
по правилам workspace (`AGENTS.md` корня MyWorkSpaceMain). Не ломать
существующий CMake/MSVC CLI.

---

## 2. Жёсткие ограничения (не нарушать)

1. **Не копировать** парсинг манифестов, SHA-256, staging/rename в форму.
   Вся логика — через `RestoreServices` / интерфейсы ядра.
2. **Не линковать** `BackupRestoreCore.lib` из MSVC в RAD Studio напрямую.
   Варианты поставки ядра в Builder (выбрать один и зафиксировать в коде):
   - **предпочтительно:** скомпилировать исходники Core в `.cbproj`
     (те же `.cpp`, без Boost/Asio/SQLite);
   - или DLL с тонким C/C++ API-обёртчиком над `createMirrorHistoryRestoreServices()`.
3. Публичные заголовки ядра **без nlohmann** — не тащить JSON в UI-слой.
4. По умолчанию восстанавливать в **новый каталог** (`D:\Recovered\...`),
   не поверх живых `D:\BASES`. Overwrite — только явная галочка + предупреждение.
5. Перед restore показывать предупреждение: остановить программы и сетевых
   пользователей баз.
6. Не менять write-path `BackupService` / `MirrorHistory.cpp` без отдельной задачи.
7. Не коммитить production-пути и секреты; тесты — только temp fixtures.

---

## 3. Обязательные файлы ядра

Читать перед кодированием UI:

| Файл | Зачем |
|---|---|
| [`RestoreTypes.h`](../src/Backup/Restore/RestoreTypes.h) | DTO для гридов |
| [`RestoreInterfaces.h`](../src/Backup/Restore/RestoreInterfaces.h) | API + фабрика |
| [`MirrorHistoryStore.*`](../src/Backup/Restore/MirrorHistoryStore.cpp) | как резолвятся файлы |
| [`MirrorHistoryRestore.cpp`](../src/Backup/Restore/MirrorHistoryRestore.cpp) | verify/restore/staging |
| [`tools/backup_restore/main.cpp`](../tools/backup_restore/main.cpp) | сценарии команд = сценарии UI |
| [`CliFormat.h`](../tools/backup_restore/CliFormat.h) | колонки таблиц (зеркало для Grid) |

Фабрика:

```cpp
RestoreServices services = createMirrorHistoryRestoreServices();
// services.scanner / points / files / planner / verifier / executor
```

Все указатели в `RestoreServices` указывают на **один** shared объект —
время жизни: на период работы формы (создать при Open root / в конструкторе).

---

## 4. Рекомендуемый макет окна

```text
+------------------------------------------------------------------+
| Корень бэкапа: [ F:\AutoPadEconomicalBackups     ] [Обзор...] [Сканировать] |
+------------------+---------------------------+-------------------+
| Цели             | Точки восстановления      | Файлы точки       |
| (ListBox/ListView)| (StringGrid / ListView)   | (StringGrid, multi)|
| BASES            | DATE TIME TIER COMPLETE…  | PATH SIZE STATUS… |
| BASES_PRD        | current ...               | ARCHIVE.db3 ...   |
| F12              | every_3min ...            | keep.txt ...      |
+------------------+---------------------------+-------------------+
| Куда: [ D:\Recovered\BASES              ] [Обзор...] [ ] Перезаписать |
| [Проверить] [План] [Восстановить всё] [Восстановить выбранные] [Отмена] |
| Прогресс: [##########........]  phase / file i/n / path                 |
+------------------------------------------------------------------+
```

Минимальные контролы:

- `Edit`/`Button` — backup root
- `ListBox`/`ListView` — targets (`display_name`, hint = `id` / `root_path`)
- Grid точек — колонки как в CLI `points`
- Grid файлов — колонки как в CLI `files`, multi-select
- `Edit` destination + checkbox overwrite
- Кнопки Verify / Plan (опционально диалог) / Restore all / Restore selected
- `TProgressBar` + label; кнопка Cancel выставляет флаг в `IRestoreProgress`

---

## 5. Маппинг UI → API

### 5.1 Сканирование корня

```text
OnScan:
  error.clear()
  targets = services.scanner->scanRoot(backup_root, error)
  if error: MessageDlg(error); return
  заполнить список целей (display_name; Tag/Data = индекс или id)
```

### 5.2 Выбор цели → точки

```text
OnTargetSelect(target):
  points = services.points->listPoints(target, error)
  заполнить grid:
    date_local | time_local | tier | label | complete | file_count | total_size
  первая строка обычно is_current == true («Текущее состояние»)
```

Подсветка: `complete == false` — warning (жёлтый); не блокировать выбор.

### 5.3 Выбор точки → файлы

```text
OnPointSelect(point):
  files = services.files->listFiles(point, error)
  заполнить grid:
    relative_path | size | resolve_status | captured_at | method
```

Отображение статуса:

| `RestoreResolveStatus` | Текст в UI |
|---|---|
| `InCurrent` | current |
| `InObjects` | objects |
| `Missing` | missing (красный) |

Если есть `Missing` — Restore должен быть недоступен или показывать ошибку
после Verify (ядро и так упадёт на missing).

### 5.4 План (опционально перед restore)

```text
path_filter = выбранные relative_path или {}
entries = services.planner->plan(point, path_filter, error)
```

Показать суммарный размер и число missing.

### 5.5 Verify

```text
ok = services.verifier->verify(point, path_filter, &progress, error)
```

### 5.6 Restore

```text
RestoreRequest req;
req.point = selected_point;
req.destination = destination_path;   // обязателен
req.overwrite = checkbox_overwrite;
req.path_filter = selected_paths;     // пусто = все файлы точки

ok = services.executor->restore(req, &progress, error)
```

Поведение ядра (не менять в GUI):

- без overwrite и если `destination` существует → ошибка;
- с overwrite → старый каталог переименовывается в `*.before_restore*`;
- staging: соседний `.backuprestore.partial_*`, затем проверка SHA, затем publish.

---

## 6. Прогресс и потоки (обязательно)

`verify` / `restore` могут долго читать диск. **Не вызывать их в UI-потоке VCL.**

Рекомендуемый шаблон:

1. Класс `GuiRestoreProgress : public IRestoreProgress`.
2. Поля: `std::atomic<bool> cancel`, очереди сообщений phase/file/warning.
3. В `onPhase` / `onFile` / `onWarning` — только thread-safe запись +
   `TThread::Queue` / `Synchronize` для обновления ProgressBar/Label.
4. `isCancelled()` читает atomic, выставляемый кнопкой «Отмена».
5. Рабочий поток:
   - создаёт/держит ссылку на `RestoreServices` (или использует общий instance,
     если синхронизирован мьютексом — проще **не** вызывать scan параллельно
     с restore);
   - вызывает verify/restore;
   - по завершении Queue: MessageDlg успех/ошибка, разблокировать кнопки.

Пока идёт операция: Disable Scan/Restore; Enable Cancel.

---

## 7. Строки и пути (Windows / VCL)

В ядре пути часто как `std::filesystem::path` и UTF-8 `std::string`
(`encoding::wstring_to_utf8` / `utf8_to_wstring` в Core).

В VCL:

- для контролов использовать `UnicodeString` / `std::wstring`;
- конвертировать на границе UI↔Core так же, как CLI (`tools/backup_restore/main.cpp`);
- не предполагать, что `std::string` в DTO — ANSI/OEM.

Для `display_name` в списке целей достаточно UTF-8→UnicodeString.

---

## 8. Интеграция в RAD Studio (чеклист)

1. Новый VCL проект Win32 (или Win64, если toolchain готов), отдельно от
   CMake `SearchEngine` — **не** смешивать в один `.cbproj` сервер поиска.
2. Добавить в проект исходники Core (минимум):
   - `src/Backup/FileHash.cpp`
   - `src/Backup/Restore/MirrorHistoryStore.cpp`
   - `src/Backup/Restore/MirrorHistoryRestore.cpp`
   - `src/MyUtils/Encoding.cpp`
   - include: `src/`, `include/`, `lib/` (nlohmann header-only)
3. C++20, Unicode, без Boost для этой цели.
4. Форма реализует только orchestration + `IRestoreProgress`.
5. Smoke вручную:
   - открыть disposable копию / тестовый fixture store;
   - scan → выбрать цель → historical point → verify → restore в temp;
   - проверить содержимое восстановленного `ARCHIVE`/файлов;
   - проверить отказ overwrite и успешный overwrite с `*.before_restore`.

Альтернатива на первом прототипе (хуже, но допустимо как stub): GUI зовёт
`BackupRestore.exe` как процесс. **Не** оставлять это финальным решением —
пользователь явно хотел классы/интерфейсы ядра.

---

## 9. Критерии приёмки GUI

- [ ] Указание backup root + сканирование показывает все mirror-цели.
- [ ] Таблица точек: дата, время, tier, complete, files, size; current сверху/отмечен.
- [ ] Таблица файлов обновляется при смене точки; статусы current/objects/missing.
- [ ] Restore all и Restore selected (path_filter) работают через Core.
- [ ] Verify перед restore (кнопка или автоматически с подтверждением).
- [ ] Progress + Cancel на длинной операции.
- [ ] Предупреждение про остановку приложений и безопасный `--to`.
- [ ] Overwrite только с явного согласия; иначе ошибка ядра понятна пользователю.
- [ ] Нет парсинга JSON/манифестов в unit’е формы.
- [ ] Не требует PowerShell на целевой машине.

---

## 10. Вне объёма (пока не делать)

- Поддержка snapshot (`snapshots\...\data`)
- Автоподмена живых `D:\BASES` без выбора Recovered-каталога
- Упаковка GUI в portable BackupService-пакет
- Изменение формата `mirror_history` или службы BackupService
- Переписывание Core на Delphi

---

## 11. Порядок работ для агента

1. Прочитать этот документ и `RestoreInterfaces.h` / `RestoreTypes.h`.
2. Собрать CLI (`BackupRestore`) и один раз пройти сценарий руками на fixture
   (см. `BackupRestoreTests.cpp`) — эталон колонок и ошибок.
3. Создать VCL-форму по макету §4.
4. Подключить исходники Core в `.cbproj` (или DLL-обёртку).
5. Реализовать Scan → Targets → Points → Files без restore.
6. Добавить `GuiRestoreProgress` + фоновый поток.
7. Verify / Restore all / Restore selected + overwrite UX.
8. Прогнать критерии §9 на temp-данных; не трогать production `D:\`/`F:\`
   без явного запроса пользователя.

---

## 12. Быстрые сниппеты

Создание сервисов:

```cpp
#include "Backup/Restore/RestoreInterfaces.h"

RestoreServices g_services = createMirrorHistoryRestoreServices();
```

Прогресс-заглушка (как в CLI, потом заменить на VCL):

```cpp
struct GuiProgress : IRestoreProgress {
    std::atomic<bool> cancel{false};
    void onPhase(const std::string& name) override { /* Queue to UI */ }
    void onFile(size_t i, size_t n, const std::string& path) override { /* Queue */ }
    void onWarning(const std::string& message) override { /* Queue */ }
    bool isCancelled() const override { return cancel.load(); }
};
```

Restore выбранных:

```cpp
RestoreRequest req;
req.point = selected_point;
req.destination = std::filesystem::path(destination_wstr);
req.overwrite = overwrite_checkbox;
req.path_filter = selected_relative_paths; // empty = all
std::string error;
bool ok = g_services.executor->restore(req, &progress, error);
```
