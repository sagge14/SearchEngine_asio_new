# Сборка SearchEngine на Windows

Проект поддерживает две целевые архитектуры через CMake Presets:

- `windows-x64` — Visual Studio 2022, x64;
- `windows-x86` — Visual Studio 2022, Win32.

Для каждой архитектуры есть отдельные Debug и Release build presets.
Старые тесты по умолчанию отключены (`BUILD_TESTS=OFF`).

## Быстрый старт

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-debug
```

Для 32-битной сборки замените `x64` на `x86`. Release собирается заменой
`debug` на `release`.

Не используйте один build-каталог для разных архитектур или генераторов.
Presets размещают их раздельно в `out/build/`.

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
