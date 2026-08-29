ZagEditor {{ARCHITECTURE}} — переносимый комплект
================================================

Архитектура: {{ARCHITECTURE}}
Минимальная система: {{MINIMUM_WINDOWS}}

Назначение: правка .zag (строка From=) по словарю EXPORT.INI.

1. Скопируйте ВСЮ папку комплекта на целевой компьютер.
2. При необходимости установите VC++ Runtime:
   prerequisites\{{VC_REDIST_FILE}}
3. Замените data\EXPORT.INI на рабочий словарь (production EXPORT.INI).
4. Отредактируйте data\zag_editor.ini (input_dirs, backup, recursive).
5. Запустите Run-ZagEditor.bat
   или: app\ZagEditor.exe --config data\zag_editor.ini
   одноразовый проход: Run-ZagEditor.bat --once

Проверка целостности пакета: Verify-Package.bat

Логи: logs\zag_editor\ рядом с рабочей директорией запуска.
Рабочий словарь и INI намеренно не входят в SHA-256-защиту пакета —
их правят под конкретную машину.
