## ZagEditor

Небольшая утилита для правки `.zag` файлов: заменяет значение первой строки `From=` по словарю из `EXPORT.INI`.

### Конфиг (INI)

Файл по умолчанию: `zag_editor.ini` (в текущей рабочей директории). Можно указать `--config`.

Поддерживаемые ключи:

- `dict_path` — путь к `EXPORT.INI` (обязательно, если не задан через `--dict`)
- `recursive` — `true/false`, обход директорий рекурсивно (по умолчанию `true`)
- `backup` — `true/false`, делать копию `<file>.bak` перед заменой (по умолчанию `true`)
- `input_dirs` — список директорий, разделитель `;` (опционально)
- `input_files` — список файлов, разделитель `;` (опционально)

Пустые строки и строки, начинающиеся с `#` или `;`, игнорируются.

### Приоритеты (INI vs CLI)

- `--dict` переопределяет `dict_path` из INI
- `--recursive/--no-recursive` переопределяют `recursive`
- `--backup/--no-backup` переопределяют `backup`
- Пути, указанные в CLI, **добавляются** к `input_dirs/input_files` из INI (merge)

### Пример `zag_editor.ini`

```ini
dict_path=D:\BASES_PRD\EXPORT.INI
recursive=true
backup=true
input_dirs=D:\TEMP_ZAG;D:\INBOX
input_files=D:\one.zag
```

### CLI

```text
ZagEditor --config zag_editor.ini --dict D:\BASES_PRD\EXPORT.INI --no-backup  D:\TEMP_ZAG  D:\one.zag
```

