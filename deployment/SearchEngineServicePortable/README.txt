SearchEngineService {{ARCHITECTURE}} — переносимый комплект
==========================================================

Архитектура: {{ARCHITECTURE}}
Минимальная система: {{MINIMUM_WINDOWS}}

На целевом компьютере PowerShell не требуется. Установка, переустановка,
остановка, запуск, перезапуск и полное удаление выполняются BAT-файлами.
Нативный помощник tools\SearchEngineConfig.exe имеет ту же архитектуру, что
SearchEngine.exe. В tools\ также лежат AuthDbTool.exe (регистрация клиентов),
SearchClientTokenIssuer.exe (выпуск USB/computer-токена searchclient-auth-token.json) и
Register-AuthClientFromToken.ps1. Выпуск токена: Issue-SearchClientToken.bat
(не нужно запускать EXE вручную). Регистрация готового токена в базе:
Register-AuthClient-FromToken.bat.

Перед установкой:

1. Скопируйте ВСЮ папку комплекта на локальный диск целевого компьютера.
2. При запуске установщик спросит идентификатор экземпляра. Для серверов
   разных годов или с разными наборами индексируемых папок выберите разные
   имена, например year2025 и year2026. Для единственного сервера оставьте
   default.
3. Проверьте в data\Settings.json каталоги config.dirs, exclude_dirs,
   prm_base_dir, prd_base_dir, tlg_send_root, razn_output_dir, opis_base_dir
   и f12_base_dir. Это локальные пути, доступные службе LocalSystem по ACL.
   User mapped drives из обычной пользовательской сессии службе недоступны.
   Отсутствие feature-specific каталога (F12, OPIS, разноска, tlg_send_root)
   не мешает запуску: ошибка появится при вызове соответствующей функции.
4. У каждого экземпляра должен быть свой свободный TCP-порт.
5. Запустите Install-SearchEngineService.bat от имени администратора.

data\Settings.json является пользовательским шаблоном конкретного компьютера:
его разрешено заменять или редактировать. Контроль целостности пакета намеренно
не проверяет папку data\. Корректность JSON проверяется после диалога настройки,
на сформированном файле.

Служба работает как LocalSystem. Рабочие корни задаются в Settings.json:
tlg_send_root, razn_output_dir, opis_base_dir и f12_base_dir. Буква диска
сама по себе не доказывает, что это физический локальный том.

Нативный помощник сначала предложит язык диалога: русский выбран по умолчанию,
английский доступен как вариант 2. Затем он предложит порт, год, число
исполнительных потоков, таймаут одного файла, опцию автоматического заполнения
«краткого содержания» AutoPad PRM, место хранения каталога документов и
четыре production-корня файловой системы.
Для новой установки Enter выбирает memory — быстрый каталог в оперативной
памяти. Вариант SQLite уменьшает расход RAM на пути и метаданные. При
переустановке текущий режим предлагается по умолчанию, но вопрос показывается
снова. Все введённые значения проверяются помощником.

Экземпляр default сохраняет прежние пути Program Files\SearchEngineService и
C:\ProgramData\SearchEngineService. Экземпляр archive получает отдельную службу
SearchEngineService-archive, display name "Search Engine ASIO Server (archive)"
и каталоги Program Files\SearchEngineService-archive и
C:\ProgramData\SearchEngineService-archive.

Для автоматической установки идентификатор можно заранее записать в
ServiceInstance.cmd или передать первым аргументом, например:

  Install-SearchEngineService.bat archive
  Stop-SearchEngineService.bat archive
  Start-SearchEngineService.bat archive
  Restart-SearchEngineService.bat archive
  Uninstall-SearchEngineService.bat archive

Повторный запуск установщика заменяет Program Files и публикует новый
Settings.json из шаблона пакета с импортом поддерживаемых старых значений.
OEM866.INI и client-endpoint.txt обновляются с малым rollback. ProgramData
остаётся на месте: индекс, авторизация, messages, logs, prefix_map.json,
пользовательский ignore.txt и неизвестные runtime-файлы не переносятся и не
удаляются. Откат не копирует многогигабайтный индекс. Необязательный export —
отдельная страховка оператора; отказ от export больше не означает удаление
ProgramData. Полное удаление ProgramData выполняется только явным uninstall.

Управление:

  Stop-SearchEngineService.bat
  Start-SearchEngineService.bat
  Restart-SearchEngineService.bat
  Uninstall-SearchEngineService.bat
  Issue-SearchClientToken.bat
  Register-AuthClient-FromToken.bat
  sc query SearchEngineService

Stop и Start — штатная остановка и последующий запуск службы (новый процесс),
а не Windows Pause/Continue. После Stop → Start служба заново читает
C:\ProgramData\SearchEngineService[-instance]\Settings.json. Остановленная
служба с Automatic/Delayed Start снова запустится после перезагрузки Windows;
для долговременного отключения отдельно переведите Startup Type в Manual или
Disabled. Изменение порта или document_catalog_storage в Settings.json
подхватывается новым процессом, но
правило Windows Firewall, client-endpoint.txt и база клиента этими скриптами
не обновляются — проверьте их вручную. Start предупреждает о расхождении порта.

Для именованного экземпляра используйте его имя, например
sc query SearchEngineService-archive.

В новом клиенте этому экземпляру рекомендуется дать такой же server_id
(например archive), указать адрес компьютера и его уникальный порт для нужного
года. После установки готовая подсказка лежит в
C:\ProgramData\SearchEngineService[-instance]\client-endpoint.txt. Она не
изменяет базу клиента автоматически. Имя Windows-службы по сети не передаётся.

При запуске Uninstall без аргумента он сканирует зарегистрированные службы
SearchEngineService[-instance] и предлагает выбрать удаляемую. Затем он
предлагает полный архив либо архив настроек и логов, после чего по
подтверждению полностью удаляет выбранную службу, firewall rule, программу и
данные, включая оставшиеся rollback-каталоги этого экземпляра. Каталоги
удаляются с повторными попытками после остановки службы; успех не сообщается,
пока Program Files или ProgramData ещё существуют. Регистрация службы удаляется
только после успешной очистки файлов.

Если после старого или прерванного удаления регистрация службы уже отсутствует,
но её каталоги остались, новый установщик показывает точные пути и предлагает
явно удалить остатки перед чистой установкой.

Перед удалением закройте services.msc и другие окна управления службами: они
могут удерживать регистрацию в состоянии «помечена на удаление». Uninstall ждёт
фактического исчезновения регистрации и предлагает повторить проверку. Последний
диагностический отчёт сохраняется в
%TEMP%\SearchEngineService-Uninstall-last.log. Окно результата закрывается
только после нажатия 0.

Подробности: INSTALLATION_GUIDE_RU.txt
