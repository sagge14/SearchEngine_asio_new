# Правила проекта SearchEngine

Область действия — весь серверный репозиторий `SearchEngine_asio_new`. Общие
правила находятся в `D:\MyWorkSpaceMain\AGENTS.md`.

## Назначение и стек

- Репозиторий содержит сервер поиска и индексации, `BackupService` и
  `ZagEditor`.
- Стек: C++20, CMake 3.21+, MSVC 2022, Boost 1.85, OpenSSL 3, utf8proc,
  nlohmann/json и SQLite amalgamation для BackupService.
- Сервер и соседний `SearchEngine_Cient_asio` — независимые репозитории. При
  изменении протокола явно проверять совместимость обеих сторон.

## Сборка

Рекомендуемые CMake presets:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
```

Для Win32 использовать `windows-x86` и `windows-x86-release`. Сборочные
каталоги находятся под `out/build` и не должны попадать в Git.

- Основные цели: `SearchEngine`, `BackupService` и `ZagEditor`.
- `BUILD_TESTS` по умолчанию выключен. Запускать CTest только после явной
  конфигурации с тестами и не считать пустой набор тестов успешной проверкой.
- Presets содержат машинные пути к Boost и OpenSSL. Не заменять их
  workspace-путями и не скачивать зависимости автоматически.

## Данные и опасные операции

- Конфигурации BackupService могут ссылаться на рабочие каталоги и базы.
  Тестовый запуск выполнять с отдельным config и отдельным каталогом назначения.
- Не запускать BackupService против production-данных и не выполнять
  install/uninstall Windows Service без отдельного запроса.
- Изменения snapshot/cache/retention-логики проверять на disposable SQLite и
  временных каталогах, включая восстановление результата.
- Не изменять vendored SQLite в `lib\backup_sqlite` без фиксации версии,
  источника и контрольной суммы.
- Существующий untracked-каталог `.cursor\` принадлежит пользовательской среде:
  не читать его содержимое, не добавлять и не удалять.

## Проверка результата

- Для серверного кода собрать затронутую цель и проверить её CLI/smoke-сценарий.
- Для BackupService дополнительно проверить `--once`, код возврата и целостность
  копии только на тестовых данных.
- Проверить, что логи, build output, runtime config и резервные копии не попали
  в staged diff.
