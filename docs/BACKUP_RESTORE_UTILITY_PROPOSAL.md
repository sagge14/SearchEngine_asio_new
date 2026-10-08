# Утилита `BackupRestore` (CLI)

Статус: **консольная версия реализована** (`BackupRestore` + библиотека
`BackupRestoreCore`). Графический интерфейс (RAD Studio) планируется поверх
тех же интерфейсов ядра.

Руководство агенту по GUI: [`BACKUP_RESTORE_GUI_AGENT_GUIDE.md`](BACKUP_RESTORE_GUI_AGENT_GUIDE.md).

Утилита работает с хранилищем `mirror_history` только на чтение до стадии
копирования во временный каталог назначения.

## Сборка

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --target BackupRestore
```

Опция CMake: `BUILD_BACKUP_RESTORE` (по умолчанию ON).

Цель `BackupRestore` линкует статическую библиотеку `BackupRestoreCore`
(ядро для CLI и будущего GUI). На Drive/portable выкладывается только
`BackupRestore.exe`, не `.lib`.

После Release-пересборки в presets `windows-x64` / `windows-x86` /
`windows7-x86` PostBuild вызывает `New-BackupRestorePackage.ps1` и при
заданном `WORKSPACE_RELEASE_CLOUD_ROOT` публикует ZIP в
`...\BackupRestore\<yyyy.MM.dd>\`.

```powershell
.\scripts\New-BackupRestorePackage.ps1 -Architecture x64
.\scripts\Build-BackupRestorePackage.ps1 -Architecture x86-modern
```

Локальный пакет: `out\package\BackupRestore-x64\` (`app\BackupRestore.exe`,
VC++ Redistributable, `Run-BackupRestore.bat`).

## Интерфейсы ядра (задел под GUI)

Заголовки без nlohmann:

- `src/Backup/Restore/RestoreTypes.h` — DTO целей, точек, файлов, плана
- `src/Backup/Restore/RestoreInterfaces.h` — scanner / catalog / planner /
  verifier / executor + `IRestoreProgress`
- Фабрика: `createMirrorHistoryRestoreServices()`

Будущий GUI может показывать:

- слева — `IBackupStoreScanner::scanRoot`
- таблицу точек — `IRestorePointCatalog::listPoints` (дата, время, tier,
  complete, файлы, размер)
- состав точки — `IRestoreFileCatalog::listFiles`
- выбор путей и каталога — `RestoreRequest` + `IRestoreExecutor::restore`

## Консольный интерфейс

```text
BackupRestore targets  --root F:\AutoPadEconomicalBackups
BackupRestore points   --root F:\AutoPadEconomicalBackups --target BASES
BackupRestore files    --point <manifest.json>
BackupRestore show     --point <manifest.json>
BackupRestore plan     --point <manifest.json> [--path rel]...
BackupRestore verify   --point <manifest.json> [--path rel]...
BackupRestore restore  --point <manifest.json> --to D:\Recovered\BASES
                       [--overwrite] [--path rel]...
BackupRestore restore  --root F:\AutoPadEconomicalBackups --target BASES
                       --latest --to D:\Recovered\BASES [--overwrite]
```

`plan` ничего не пишет на диск: показывает источник каждого файла
(`current` или `objects`) и отсутствующие объекты.

`verify` проверяет наличие, размер и SHA-256.

`restore` собирает данные в соседний `.backuprestore.partial_*`, проверяет
хеши и публикует в `--to`. Существующий каталог не затирается без
`--overwrite` (при overwrite старый каталог переименовывается в
`*.before_restore`).

Коды выхода: `0` OK, `1` ошибка операции, `2` аргументы.

## Возможный графический интерфейс

Главное окно:

- слева — найденные цели (`BASES`, `BASES_PRD`, `F12`, `OTPRAVKA`, `TVRD`);
- в центре — точки, отсортированные по дате, с колонками «период»,
  «полнота», «файлов», размер;
- справа — состав выбранной точки и результат проверки;
- снизу — каталог назначения и кнопки «Проверить», «Показать план»,
  «Восстановить всё», «Восстановить выбранные файлы».

Особый пункт «Текущее состояние» — точка `current` / `current\data`.

Перед записью в рабочий каталог окно должно предупредить, что программы
и сетевые пользователи должны быть отключены. Безопасный сценарий по
умолчанию — восстановление в новый каталог с последующей ручной заменой.

Для RAD Studio не линкуйте MSVC `.lib` напрямую: позже — те же исходники
Core в `.cbproj` или DLL-обёртка над интерфейсами.
