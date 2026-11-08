# Выпуск токенов SearchClientTokenIssuer

Инженерная справка по CLI, формату и сборке. Пользовательские действия:
[токен другого ПК](../../tutorials/Подробные_руководства/Токен_удалённого_ПК.html),
[общий доступ](../../tutorials/Подробные_руководства/Общий_доступ_к_серверам.html),
[перевыпуск](../../tutorials/Подробные_руководства/Утрачен_пароль_ключа_издателя.html).

Утилита Windows выпускает подписанный searchclient-auth-token.json с типом
устройства usb или computer. ПК-токен автоматически ищется только в
%LOCALAPPDATA%\SearchEngine\searchclient-auth-token.json текущего пользователя.
Процесс под другой учётной записью использует другой профиль. Подходящий
существующий токен можно перенести сюда без новой подписи и регистрации.

## Формат и проверка

- Хранилище RSA-2048: %ProgramData%\SearchClientTokenIssuer\keys по умолчанию.
  Закрытый ключ private.enc.pem — PKCS#8, AES-256-CBC.
- Токен: format_version: 1; signature.alg: RS256.
- Подписывается UTF-8-строка client_id + "\n" + client_name + "\n" +
  device_type + "\n" + device_id + "\n".
- Сервер проверяет подпись по issuer-public.pem рядом с auth_clients.sqlite;
  при отсутствии — public.pem общего хранилища. Клиент проверяет аппаратный
  номер USB или SMBIOS UUID и передаёт подпись в AUTHENTICATE_V1.
- Строки идентичности — печатные ASCII-символы. expires_at пока не проверяется.

Неподписанная заявка: format: searchclient-auth-request, версия 1 и четыре
поля идентичности. Она не принимается как готовый токен клиентом и AuthDbTool.
Создание заявки не создаёт и не открывает хранилище; подпись сохраняет UUID
заявки, не подменяя его UUID компьютера издателя. Исходная заявка сохраняется.

## Параметры CLI

| Параметр | Назначение |
|---|---|
| --language ru/en/auto | Язык интерфейса |
| --init-keystore | Создать хранилище RSA-2048 |
| --export-public PATH | Экспортировать открытый ключ; в каталог как issuer-public.pem |
| --show-computer-id | Показать UUID без выпуска токена |
| --create-request | Создать неподписанную заявку ПК |
| --sign-request PATH | Подписать заявку существующим ключом |
| --id ID, --name NAME | Идентификатор и имя клиента |
| --device-type usb/computer | Тип устройства |
| --drive E: | Носитель USB |
| --output PATH | Файл или существующий каталог; при отсутствии диалог сохранения |
| --defaults PATH | JSON начальных значений |
| --keystore PATH | Каталог хранилища |
| --password-env NAME | Имя переменной окружения с паролем |
| --allow-manual-serial S | Явная подмена определённого USB-номера |
| --yes | Пропустить подтверждения подписи/замены |

BAT и интерактивный режим предлагают русский по Enter/1, английский по 2.
CLI без явного языка использует английский; BAT /quiet пропускает язык и паузу.
Непустые имя/идентификатор и пароль обязательны; издатель и примечания
необязательны. Пустой Enter не принимает учебное имя за пользователя.
Коды завершения: 0 — успех, 1 — ошибка, 2 — отмена, 3 — нет подходящего носителя/номера.

## Учебные примеры

Используйте отдельные каталоги; не подставляйте рабочие ключи и базы для теста.

    SearchClientTokenIssuer.exe --create-request --name Ivanov --output E:\transfer
    SearchClientTokenIssuer.exe --sign-request E:\transfer\searchclient-auth-request.json --output E:\transfer --keystore E:\issuer\keys
    SearchClientTokenIssuer.exe --export-public E:\test-service\data
    SearchClientTokenIssuer.exe --show-computer-id
    SearchClientTokenIssuer.exe --init-keystore --password-env TOKEN_ISSUER_PASSWORD
    SearchClientTokenIssuer.exe --device-type usb --drive E: --name Ivanov --id C-001 --yes --password-env TOKEN_ISSUER_PASSWORD
    SearchClientTokenIssuer.exe --device-type computer --name Ivanov --id C-001 --output E:\transfer --yes --password-env TOKEN_ISSUER_PASSWORD
    AuthDbTool.exe --db E:\test-service\data\auth_clients.sqlite add-from-token --token E:\transfer\searchclient-auth-token.json

У заявки можно явно задать --id; обычный идентификатор ПК — PC-<SMBIOS UUID>.
Регистрация и выпуск отдельны. Register-AuthClient-FromToken.bat предлагает
ПК-токен, USB или ручной файл; /token PATH пропускает меню источника.
PowerShell scripts/Register-AuthClientFromToken.ps1 принимает -TokenPath.

## Перевыпуск и сборка

Пункт 6 Setup-Access готовит новый ключ и подписи каталога устройств, пункт 7
переключает серверы, пункт 8 перевыпускает USB-токен; у Issue это пункт 5.
Копии старых ключей не создаются, базы разрешений сохраняются. Прежний пароль
не сбрасывается. Сервер принимает один ключ одновременно. USB-перевыпуск
проверяет аппаратный номер и сохраняет четыре поля идентичности.

    SearchClientTokenIssuer.exe --reissue-authority E:\test\catalogue.json --authority-output E:\test\access-authority.json --keystore E:\test\keys
    SearchClientTokenIssuer.exe --reissue-token E:\test\old-token.json --drive E: --keystore E:\test\keys

keys.reissue-pending содержит только новые ключи и новое состояние; повтор
операции продолжает с ними и новым паролем. При успехе каталог удаляется.

    cmake --preset windows-x64
    cmake --build --preset windows-x64-release --target SearchClientTokenIssuer

Пакет содержит tools\SearchClientTokenIssuer.exe, начальный JSON и OpenSSL
libcrypto рядом с утилитой. Закрытые ключи в распространяемый комплект не входят.
