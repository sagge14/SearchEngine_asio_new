BackupRestore {{ARCHITECTURE}} — переносимый комплект
===================================================

Архитектура: {{ARCHITECTURE}}
Минимальная система: {{MINIMUM_WINDOWS}}

Назначение: восстановление из хранилища mirror_history (профиль economical
BackupService), без установки службы.

1. Скопируйте ВСЮ папку комплекта на целевой компьютер.
2. При необходимости установите VC++ Runtime:
   prerequisites\{{VC_REDIST_FILE}}
3. Запустите справку:
   Run-BackupRestore.bat --help
4. Типичный сценарий (корень бэкапа → цель → восстановление):

   Run-BackupRestore.bat targets  --root F:\AutoPadEconomicalBackups
   Run-BackupRestore.bat points   --root F:\AutoPadEconomicalBackups --target BASES
   Run-BackupRestore.bat restore  --root F:\AutoPadEconomicalBackups --target BASES --latest --to D:\Recovered\BASES

По умолчанию восстанавливайте в новый каталог, не поверх живых баз.
Перезапись только с явным --overwrite.

Проверка целостности пакета: Verify-Package.bat

BackupRestoreCore.lib в комплект не входит — это внутренняя библиотека сборки.
Документация: docs\BACKUP_RESTORE_UTILITY_PROPOSAL.md в исходниках проекта.
