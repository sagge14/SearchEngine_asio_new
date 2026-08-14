# Сборка SearchEngine на Windows

Проект поддерживает две целевые архитектуры через CMake Presets:

- `windows-x64` — Visual Studio 2022, x64;
- `windows-x86` — Visual Studio 2022, Win32.

Для каждой архитектуры есть отдельные Debug и Release build presets.
Старые тесты по умолчанию отключены (`BUILD_TESTS=OFF`).

## Полная пакетная индексация

Параметр `config.full_index_strategy` принимает `legacy` или `batch`.
Без нового поля используется значение `batch`. Этот режим
применяется только к первичной полной сборке действительно пустого индекса;
последующие точечные изменения выполняются существующей legacy-логикой. Для
сравнения двух алгоритмов используйте отдельные чистые тестовые index DB.

- `batch_reader_threads`: для HDD обычно `1`; для SSD/NVMe значение следует
  подбирать измерением на тестовом корпусе;
- `batch_indexer_threads`: `0` означает `hardware_concurrency`;
- `batch_queue_memory_mb`: общий лимит payload в I/O-очередях. Он не включает
  64-КиБ read-buffer каждого reader, текущий блок indexer, локальные словари и
  RAM merge. Допустимый диапазон — 16..2048 MiB, в том числе на Win32.

## Каталог документов

Параметр `config.document_catalog_storage` не зависит от
`full_index_strategy` и принимает:

- `memory` — значение по умолчанию и поведение старых `Settings.json`; пути и
  метаданные загружаются в `DocPaths`, а SQLite `docs` продолжает обновляться;
- `sqlite` — постоянный полный каталог путей не загружается в RAM. Поиск
  получает `path` и `deleted` пакетно только для итогового top-N.

Смена режима применяется после перезапуска и не меняет `docId`. Исправная
таблица `docs` позволяет переключаться без повторной индексации. Дубликаты
`docs.path`, отсутствующие строки `docs` для postings и ошибки SQLite являются
ошибкой запуска; автоматическое удаление или fallback на пустой каталог не
выполняются.

## Быстрый старт

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-debug
```

Для 32-битной сборки замените `x64` на `x86`. Release собирается заменой
`debug` на `release`.

Не используйте один build-каталог для разных архитектур или генераторов.
Presets размещают их раздельно в `out/build/`.

## App-версия и release-пакеты

Канон версии продукта — `app-version.json` (SearchEngineService) и
`app-version.<Product>.json` для BackupService / ZagEditor / BackupRestore.

- Формат: `major.minor.patch`
- PE: `ProductVersion=A.B.C`, `FileVersion=A.B.C.0`
- Generated resources: `cmake/generated/<Product>/*_version.rc`
- Имена EXE **не** меняются (`SearchEngine.exe`, не `SearchEngine_v001.exe` в
  portable/ZIP; legacy `copy_with_version.ps1` к release naming не относится)

Продукты с отдельным version manifest:

| Продукт | JSON | EXE |
|---|---|---|
| SearchEngineService | `app-version.json` | `SearchEngine.exe`, `SearchEngineConfig.exe` (общая версия) |
| BackupService | `app-version.BackupService.json` | `BackupService.exe` |
| ZagEditor | `app-version.ZagEditor.json` | `ZagEditor.exe` |
| BackupRestore | `app-version.BackupRestore.json` | `BackupRestore.exe` |

### Кто повышает patch

| Сценарий | Bump patch | PE / пакет |
|---|---|---|
| IDE / CMake **Release** Build/Rebuild цели в packagable preset (`windows-x64`, `windows-x86`, `windows7-x86`) при `SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=ON` | Да, **один раз до компиляции** (`Ensure-ReleaseVersionBump.ps1` → sync `.rc`/`.h`) | POST_BUILD упаковывает **ту же** новую версию |
| `Build-*Package.ps1` | Да, один раз в начале скрипта (до configure/build); `Architecture=All` — одна версия на все архитектуры | Затем build + `New-*Package`; CMake bump отключён (`PACKAGE_ON=OFF` и/или `SEARCHENGINE_VERSION_BUMP_MODE=skip`) |
| `Build-*Package.ps1 -SkipVersionBump` | Нет | Текущая версия в PE и пакете |
| `New-*Package.ps1` (повторная упаковка) | Нет | Текущая версия из JSON / PE |
| Debug / RelWithDebInfo / MinSizeRel | Нет | Пакет не публикуется |
| `SEARCHENGINE_PACKAGE_ON_RELEASE_BUILD=OFF` | Нет (IDE/CMake path) | POST_BUILD упаковки нет |

Важно: bump **нельзя** делать только в POST_BUILD — EXE уже собран со старым
VERSIONINFO. При ошибке компиляции после bump пакет и cloud ZIP не создаются
(POST_BUILD не запускается).

Release через скрипт (bump → build → package → cloud при настроенном
`WORKSPACE_RELEASE_CLOUD_ROOT`):

```powershell
.\scripts\Build-SearchEngineServicePackage.ps1 -Architecture x64
```

Без увеличения patch: `-SkipVersionBump`. Без облака: `-SkipCloudPublish`.

Локальный пакет и ZIP на Drive:

```text
out\package\<A.B.C>\SearchEngineService-x64\
Releases\SearchEngineService\<A.B.C>\SearchEngineService-x64-<A.B.C>.zip
```

## Требуемые локальные зависимости

По умолчанию CMake использует:

| Зависимость | x64 | x86 |
|---|---|---|
| Boost 1.85 | `C:/Boost/windows` | `C:/Boost/windows32` |
| OpenSSL 3 | `C:/Program Files/OpenSSL-Win64` | `C:/Program Files (x86)/OpenSSL-Win32` |
| utf8proc | собирается из `utf8proc-2.10.0/utf8proc-2.10.0` |

Пути можно изменить переменными:

- `SEARCHENGINE_BOOST_ROOT`;
- `SEARCHENGINE_BOOST_COMPILER`;
- `SEARCHENGINE_OPENSSL_ROOT`;
- `SEARCHENGINE_UTF8PROC_SOURCE_DIR`.

CMake проверяет, что у бинарных Boost-компонентов есть обе конфигурации и
что их архитектура совпадает с целью.

## Согласованность Debug/Release

Весь MSVC-граф сборки использует один CRT:

- Debug: `/MDd`, `_ITERATOR_DEBUG_LEVEL=2`;
- Release и RelWithDebInfo: `/MD`, `_ITERATOR_DEBUG_LEVEL=0`.

Значение `_ITERATOR_DEBUG_LEVEL` не переопределяется вручную. Boost выбирается
через imported targets, поэтому Debug линкуется с `-gd-` библиотеками, а
Release — с обычными. OpenSSL находится стандартным `FindOpenSSL`, который
выбирает каталоги `MDd` и `MD` соответственно.

Debug и RelWithDebInfo создают полные PDB в `out/build/<preset>/symbols/`.
В Debug явно отключены оптимизация и inline-подстановка (`/Od /Ob0`).

Старый тестовый проект не входит в стандартный граф сборки. При необходимости
его можно включить вручную через `-DBUILD_TESTS=ON`.

## Пересборка Boost

Обе архитектуры Boost можно пересобрать согласованно с VS 2022:

```powershell
.\cmake\BuildBoost.ps1 -Architecture all -Toolset msvc-14.3
```

Скрипт собирает `variant=debug,release`, `link=static`,
`runtime-link=shared`. То есть Debug получает `/MDd` и debug runtime, Release
— `/MD`. Для каждой архитектуры используются отдельные install/build
каталоги.

## Результаты ревизии старой конфигурации

До исправления корневой CMake:

- всегда задавал `C:/Boost/windows32` и `vc142`, включая x64;
- определял `_X86_` для любого MSVC target;
- всегда включал `/arch:AVX2`;
- линковал единственный `utf8proc_static.lib`, который является x86;
- использовал глобальные include/link directories и список
  `${Boost_LIBRARIES}`, из-за чего выбор конфигурации зависел от cache;
- не закреплял единый MSVC CRT для приложения, Boost и GoogleTest.

Проверка локальных Boost.Serialization библиотек показала, что сами пары
Boost корректны:

- x64 `vc143`: Debug `/MDd`, iterator level 2; Release `/MD`, level 0;
- x86 `vc142`: Debug `/MDd`, iterator level 2; Release `/MD`, level 0.

Следовательно, основная проблема была в выборе архитектуры/конфигурации в
CMake и в x86-only utf8proc, а не в debug level локального Boost.
