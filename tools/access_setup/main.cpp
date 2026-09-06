#include "AccessBundle.hpp"
#include "TokenLoader.hpp"
#include "TokenDocument.hpp"
#include "ComputerTokenPath.hpp"
#include "ComputerIdentity.hpp"
#include "CryptoStub.hpp"
#include "Auth/AuthClientStore.h"
#include "Auth/IssuerPublicKeyPath.h"
#include "Auth/DeviceIdentity.h"
#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using access_setup::Json;
namespace {
struct Cancelled {};
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!s.empty() && n == 0) throw std::runtime_error("Invalid UTF-8");
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n); return w;
}
std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr); return s;
}
void Say(const std::wstring& text) {
    DWORD mode{}, count{}; HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(output, &mode)) WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &count, nullptr);
    else { const auto bytes = Utf8(text); WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr); }
}
std::wstring Line() {
    DWORD mode{}, count{}; HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(input, &mode)) {
        wchar_t buffer[4096]{};
        if (!ReadConsoleW(input, buffer, 4095, &count, nullptr)) throw Cancelled{};
        std::wstring s(buffer, count); while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back(); return s;
    }
    std::string s; if (!std::getline(std::cin, s)) throw Cancelled{}; return Wide(s);
}
bool Yes(const std::wstring& text) {
    Say(text + L"\n1 — Да\n0 — Нет\nВыбор: ");
    for (;;) { const auto answer = Line(); if (answer == L"1") return true; if (answer == L"0") return false; Say(L"Введите 1 или 0: "); }
}
fs::path ExeDirectory() {
    wchar_t path[32768]{}; const auto n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768) throw std::runtime_error("Cannot locate executable"); return fs::path(path).parent_path();
}
fs::path Profile() {
    const auto path = token_issuer::StandardComputerTokenDirectory();
    if (!path) throw std::runtime_error("Cannot locate Windows user profile"); return *path;
}
fs::path LocalToken() { return Profile() / token_issuer::kTokenFileName; }
fs::path PendingRequest() { return Profile() / "access-setup-request.json"; }
fs::path StatePath() { return Profile() / "access-authority.json"; }
std::string ReadBytes(const fs::path& path);
void RunIssuer(const std::vector<std::wstring>& arguments);
void RequireAdminNow() {
    if (!IsUserAnAdmin()) throw std::runtime_error("Run Setup-Access.bat as administrator to configure servers");
}
bool AuthorityKeyConflict() {
    if (!fs::exists(StatePath())) return false;
    const auto state = access_setup::ReadDocument(StatePath());
    const auto paths = token_issuer::ResolveKeystorePaths(token_issuer::DefaultKeystoreRoot());
    if (!token_issuer::KeystoreExists(paths)) return true;
    return !access_setup::AuthorityUsesPublicKey(state, ReadBytes(paths.public_key));
}
void ReissueAuthorityKeys() {
    RequireAdminNow();
    const auto keys = token_issuer::DefaultKeystoreRoot();
    const auto retiredState = access_setup::RetireToBackup(StatePath());
    const auto retiredKeys = access_setup::RetireToBackup(keys);
    RunIssuer({L"--init-keystore", L"--keystore", keys.wstring()});
    Say(L"Ключи перевыпущены. Старая система отложена, её токены больше не действуют.\n");
    if (!retiredState.empty()) Say(L"Прежняя главная система: " + retiredState.wstring() + L"\n");
    if (!retiredKeys.empty()) Say(L"Прежние ключи: " + retiredKeys.wstring() + L"\n");
}
bool ResolveAuthorityKeyConflict(bool required) {
    if (!AuthorityKeyConflict()) return false;
    Say(L"\nСохранённая главная система подписана другим ключом, чем текущее хранилище.\n"
        L"Хранилище: " + token_issuer::DefaultKeystoreRoot().wstring() + L"\n"
        L"Старые токены и ответы больше не подойдут. Второй ПК нужно провести заново.\n");
    if (!Yes(L"Перевыпустить ключи и начать новую систему?")) {
        if (required) throw Cancelled{};
        return false;
    }
    ReissueAuthorityKeys();
    return true;
}
std::string ReadBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("Cannot read selected file");
    std::string bytes(1024 * 1024 + 1, '\0'); f.read(bytes.data(), bytes.size());
    if (f.bad() || f.gcount() > 1024 * 1024) throw std::runtime_error("Selected file is too large");
    bytes.resize(static_cast<std::size_t>(f.gcount())); return bytes;
}
std::string NewId() { GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create request ID"); wchar_t s[40]{}; StringFromGUID2(id, s, 40); return *auth::NormalizeComputerUuid(Utf8(s)); }
fs::path ChooseFile(bool save, const wchar_t* title, const wchar_t* name = L"access-request.json") {
    wchar_t buffer[32768]{}; if (save) wcsncpy_s(buffer, name, _TRUNCATE);
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = GetConsoleWindow();
    ofn.lpstrTitle = title; ofn.lpstrFile = buffer; ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"Файлы настройки (*.json)\0*.json\0Все файлы (*.*)\0*.*\0"; ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) { if (CommDlgExtendedError()) throw std::runtime_error("Cannot open file dialog"); throw Cancelled{}; }
    return buffer;
}
std::wstring Quote(const std::wstring& s) {
    std::wstring out = L"\""; unsigned slashes = 0;
    for (wchar_t c : s) { if (c == L'\\') { ++slashes; continue; } out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; out += c; }
    out.append(slashes * 2, L'\\'); return out + L"\"";
}
void RunIssuer(const std::vector<std::wstring>& arguments) {
    const fs::path exe = ExeDirectory() / "SearchClientTokenIssuer.exe";
    std::wstring command = Quote(exe.wstring()) + L" --language ru";
    for (const auto& arg : arguments) command += L" " + Quote(arg);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Cannot start token issuer");
    CloseHandle(process.hThread); WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code{}; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    if (code == 2) throw Cancelled{}; if (code != 0) throw std::runtime_error("Token issuer did not complete; see the message above");
}
std::string ChooseRole() {
    Say(L"Что будет работать на этом ПК?\n1 — Клиент и сервер\n2 — Только клиент\n3 — Только сервер\n0 — Отмена\nВыбор: ");
    for (;;) { const auto answer = Line(); if (answer == L"1") return "client_server"; if (answer == L"2") return "client"; if (answer == L"3") return "server"; if (answer == L"0") throw Cancelled{}; Say(L"Введите 1, 2, 3 или 0: "); }
}
bool HasClient(const std::string& role) { return role != "server"; }
bool HasServer(const std::string& role) { return role != "client"; }
struct Service { std::wstring name; fs::path data; };
struct ScHandle { SC_HANDLE h{}; ~ScHandle() { if (h) CloseServiceHandle(h); } };
std::vector<Service> Services() {
    ScHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)};
    if (!manager.h) throw std::runtime_error("Cannot list Windows services");
    DWORD needed{}, count{}, resume{};
    EnumServicesStatusExW(manager.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<BYTE> buffer(needed + 4096); resume = 0;
    if (!EnumServicesStatusExW(manager.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr))
        throw std::runtime_error("Cannot list Windows services");
    std::vector<Service> result; const auto rows = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) {
        std::wstring name = rows[i].lpServiceName;
        if (name != L"SearchEngineService" && name.rfind(L"SearchEngineService-", 0) != 0) continue;
        ScHandle service{OpenServiceW(manager.h, name.c_str(), SERVICE_QUERY_CONFIG)};
        if (!service.h) continue;
        DWORD size{}; QueryServiceConfigW(service.h, nullptr, 0, &size);
        std::vector<BYTE> config(size);
        if (!QueryServiceConfigW(service.h, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data()), size, &size)) continue;
        auto c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data());
        int argc{}; wchar_t** argv = CommandLineToArgvW(c->lpBinaryPathName, &argc); fs::path data;
        if (argv) { for (int j = 1; j + 1 < argc; ++j) if (std::wstring(argv[j]) == L"--data-dir") data = argv[j + 1]; LocalFree(argv); }
        if (data.empty() || !data.is_absolute() || !fs::is_directory(data)) continue;
        result.push_back({name, data});
    }
    std::sort(result.begin(), result.end(), [](const Service& a, const Service& b) { return a.name < b.name; }); return result;
}
std::vector<Service> SelectServices() {
    auto services = Services(); if (services.empty()) throw std::runtime_error("No installed SearchEngine server with a data directory was found");
    Say(L"Какие серверы этого ПК включить в общую систему?\n");
    for (std::size_t i = 0; i < services.size(); ++i) Say(std::to_wstring(i + 1) + L" — " + services[i].name + L"\n    " + services[i].data.wstring() + L"\n");
    Say(L"0 — Все перечисленные\nВыбор (один номер): ");
    for (;;) { const auto answer = Line(); if (answer == L"0") return services;
        try { std::size_t length{}; const int n = std::stoi(answer, &length); if (length == answer.size() && n > 0 && n <= static_cast<int>(services.size())) return {services[n - 1]}; } catch (...) {}
        Say(L"Введите номер из списка: "); }
}
std::vector<Service> StateServices(const Json& state) {
    const auto all = Services(); std::vector<Service> selected;
    for (const auto& name : state.at("services")) {
        const auto w = Wide(name.get<std::string>()); const auto match = std::find_if(all.begin(), all.end(), [&](const Service& s) { return s.name == w; });
        if (match == all.end()) throw std::runtime_error("An enrolled local server is no longer installed"); selected.push_back(*match);
    }
    return selected;
}
void RequireAdmin(const std::vector<Service>& services) {
    if (!services.empty() && !IsUserAnAdmin()) throw std::runtime_error("Run Setup-Access.bat as administrator to configure servers");
}
DWORD ServiceState(SC_HANDLE service) { SERVICE_STATUS_PROCESS status{}; DWORD bytes{}; if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) throw std::runtime_error("Cannot read server service status"); return status.dwCurrentState; }
void WaitState(SC_HANDLE service, DWORD state) {
    const ULONGLONG until = GetTickCount64() + 60000;
    while (ServiceState(service) != state) { if (GetTickCount64() >= until) throw std::runtime_error("Timed out waiting for the server service"); Sleep(250); }
}
void Preflight(const std::vector<Service>& services, const Json& tokens) {
    RequireAdmin(services);
    for (const auto& service : services) {
        auth::AuthClientStore store; store.open(service.data / "auth_clients.sqlite");
        for (const auto& token : tokens) {
            const auto f = auth_db::parseTokenFields(token); const auto old = store.getClient(f.client_id);
            if (old && (old->client_name != f.client_name || old->device_type != f.device_type || old->device_id != f.device_id))
                throw std::runtime_error("Client ID conflicts with another identity on a selected server");
        }
    }
}
void InstallServers(const std::vector<Service>& services, const std::string& pem, const Json& tokens) {
    access_setup::PublicKeyFingerprint(pem); for (const auto& t : tokens) if (!access_setup::VerifyToken(t, pem)) throw std::runtime_error("Untrusted token");
    Preflight(services, tokens);
    for (const auto& service : services) {
        Say(L"Настраивается сервер: " + service.name + L"\n");
        ScHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        ScHandle handle{manager.h ? OpenServiceW(manager.h, service.name.c_str(), SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr};
        if (!handle.h) throw std::runtime_error("Cannot control the selected server service");
        const DWORD initial = ServiceState(handle.h);
        if (initial != SERVICE_RUNNING && initial != SERVICE_STOPPED) throw std::runtime_error("Wait until the selected service finishes starting or stopping");
        const bool running = initial == SERVICE_RUNNING;
        if (running) { SERVICE_STATUS status{}; if (!ControlService(handle.h, SERVICE_CONTROL_STOP, &status)) throw std::runtime_error("Cannot stop selected server"); WaitState(handle.h, SERVICE_STOPPED); }
        try {
            access_setup::InstallTrust(service.data, pem, tokens);
        } catch (...) {
            if (running) StartServiceW(handle.h, 0, nullptr);
            throw;
        }
        if (running) { if (!StartServiceW(handle.h, 0, nullptr)) throw std::runtime_error("Cannot restart selected server"); WaitState(handle.h, SERVICE_RUNNING); }
        Say(L"Ключ установлен, разрешения зарегистрированы. " + std::wstring(running ? L"Служба перезапущена.\n" : L"Служба оставлена остановленной.\n"));
    }
}
Json Prepare(const std::string& role) {
    const std::string uuid = token_issuer::RequireComputerDeviceId(); Json identity = nullptr;
    if (HasClient(role)) {
        const auto output = Profile() / "access-identity-request.json";
        RunIssuer({L"--create-request", L"--output", output.wstring(), L"--yes"});
        identity = access_setup::ReadDocument(output);
    }
    Json request{{"format", "searchengine-access-request"}, {"version", 1}, {"request_id", NewId()},
                 {"computer_uuid", uuid}, {"role", role}, {"identity", identity}};
    access_setup::ValidateRequest(request); return request;
}
Json SignIdentity(const Json& request, const fs::path& keys) {
    const auto input = Profile() / "access-sign-request.json";
    const auto output = Profile() / "access-signed-token.json";
    access_setup::WriteDocument(input, request.at("identity"));
    RunIssuer({L"--sign-request", input.wstring(), L"--output", output.wstring(), L"--keystore", keys.wstring(), L"--yes"});
    return access_setup::ReadDocument(output);
}
void InstallLocalToken(const Json& token) {
    const auto fields = auth_db::parseTokenFields(token);
    if (fields.device_type != "computer" || fields.device_id != token_issuer::RequireComputerDeviceId()) throw std::runtime_error("Token belongs to another computer");
    const auto path = LocalToken();
    if (fs::exists(path) && access_setup::ReadDocument(path) != token) fs::copy_file(path, Profile() / "searchclient-auth-token.before-access.json", fs::copy_options::overwrite_existing);
    access_setup::WriteDocument(path, token);
    Say(L"Токен этого ПК установлен для текущего пользователя Windows.\n");
}
void ConfigureAuthority() {
    (void)ResolveAuthorityKeyConflict(true);
    const auto role = ChooseRole(); const auto services = HasServer(role) ? SelectServices() : std::vector<Service>{}; RequireAdmin(services);
    const auto keys = token_issuer::DefaultKeystoreRoot();
    Say(L"Главный ПК будет подписывать токены. Закрытый ключ останется здесь.\nХранилище: " + keys.wstring() + L"\n");
    if (!Yes(L"Настроить этот ПК как главный и использовать его ключ на выбранных серверах?")) throw Cancelled{};
    const auto keypaths = token_issuer::ResolveKeystorePaths(keys);
    if (!token_issuer::KeystoreExists(keypaths)) RunIssuer({L"--init-keystore", L"--keystore", keys.wstring()});
    const auto pem = ReadBytes(keypaths.public_key); access_setup::PublicKeyFingerprint(pem);
    Json tokens = Json::array();
    if (fs::exists(StatePath())) {
        const auto previous = access_setup::ReadDocument(StatePath());
        if (previous.at("computer_uuid") != token_issuer::RequireComputerDeviceId()) throw std::runtime_error("Authority state belongs to another computer");
        if (previous.at("public_key_fingerprint") != access_setup::PublicKeyFingerprint(pem)) throw std::runtime_error("Authority key changed; do not overwrite the existing client directory");
        tokens = previous.at("tokens"); for (const auto& t : tokens) if (!access_setup::VerifyToken(t, pem)) throw std::runtime_error("Untrusted token in authority directory");
    }
    Json own = nullptr;
    if (HasClient(role)) {
        if (fs::exists(LocalToken())) { const auto candidate = access_setup::ReadDocument(LocalToken()); const auto f = auth_db::parseTokenFields(candidate);
            if (f.client_id != "local-machine" && f.device_type == "computer" && f.device_id == token_issuer::RequireComputerDeviceId() && access_setup::VerifyToken(candidate, pem)) own = candidate; }
        if (own.is_null()) own = SignIdentity(Prepare(role), keys);
        access_setup::AddToken(tokens, own);
    }
    InstallServers(services, pem, tokens);
    if (!own.is_null()) InstallLocalToken(own);
    Json names = Json::array(); for (const auto& s : services) names.push_back(Utf8(s.name));
    access_setup::WriteDocument(StatePath(), {{"computer_uuid", token_issuer::RequireComputerDeviceId()}, {"role", role},
        {"public_key_fingerprint", access_setup::PublicKeyFingerprint(pem)}, {"services", names}, {"tokens", tokens}});
    Say(L"Главный ПК настроен. Теперь можно обрабатывать заявки других компьютеров.\n");
}
void CheckTransferOutput(const fs::path& path) {
    const auto key = fs::weakly_canonical(path).wstring();
    for (const auto& internal : {Profile(), token_issuer::DefaultKeystoreRoot()}) {
        const auto base = fs::weakly_canonical(internal).wstring() + L"\\";
        if (key.size() >= base.size() && _wcsnicmp(key.c_str(), base.c_str(), base.size()) == 0)
            throw std::runtime_error("Choose a separate transfer file");
    }
    if (fs::exists(path)) {
        const auto existing = access_setup::ReadDocument(path);
        const auto format = existing.value("format", "");
        if (format != "searchengine-access-request" && format != "searchengine-access-package")
            throw std::runtime_error("Choose a separate transfer file");
    }
}
void PrepareRequest() {
    const auto role = ChooseRole(); const auto request = Prepare(role);
    const auto path = ChooseFile(true, L"Сохранить заявку на флешку", L"access-request.json");
    CheckTransferOutput(path);
    access_setup::WriteDocument(PendingRequest(), request); access_setup::WriteDocument(path, request);
    Say(L"Заявка сохранена. Передайте этот один файл на главный ПК.\n");
}
void ProcessRequest() {
    if (ResolveAuthorityKeyConflict(true)) {
        Say(L"Сначала выполните пункт 1 — настройку главного ПК с новыми ключами. Затем снова выберите пункт 3.\n");
        return;
    }
    if (!fs::exists(StatePath())) throw std::runtime_error("Configure this computer as the authority first");
    auto state = access_setup::ReadDocument(StatePath());
    if (state.at("computer_uuid") != token_issuer::RequireComputerDeviceId()) throw std::runtime_error("Authority state belongs to another computer");
    const auto input = ChooseFile(false, L"Выберите заявку другого ПК"); const auto request = access_setup::ReadDocument(input); access_setup::ValidateRequest(request);
    const auto keys = token_issuer::DefaultKeystoreRoot(); const auto pem = ReadBytes(keys / "public.pem");
    if (state.at("public_key_fingerprint") != access_setup::PublicKeyFingerprint(pem)) throw std::runtime_error("Authority key changed");
    auto tokens = state.at("tokens"); for (const auto& t : tokens) if (!access_setup::VerifyToken(t, pem)) throw std::runtime_error("Untrusted token in authority directory");
    Say(L"Получена заявка. Компьютер: " + Wide(request.at("computer_uuid")) + L"\n");
    if (!request.at("identity").is_null()) Say(L"Имя клиента: " + Wide(request.at("identity").at("client_name")) + L"\n");
    if (!Yes(L"Разрешить подключение этого компьютера?")) throw Cancelled{};
    const auto services = StateServices(state); RequireAdmin(services);
    if (HasClient(request.at("role"))) {
        const auto fields = token_issuer::ParseComputerRequestDocument(request.at("identity")); bool found = false;
        for (const auto& token : tokens) { const auto old = auth_db::parseTokenFields(token);
            if (old.client_id == fields.client_id) {
                if (old.client_name != fields.client_name || old.device_type != fields.device_type || old.device_id != fields.device_id) throw std::runtime_error("Client ID conflicts with another identity");
                found = true;
            } }
        if (!found) access_setup::AddToken(tokens, SignIdentity(request, keys));
    }
    const auto package = access_setup::MakePackage(request, pem, tokens);
    const auto output = ChooseFile(true, L"Сохранить ответ на флешку", L"access-answer.json");
    CheckTransferOutput(output);
    if (fs::exists(output) && fs::equivalent(input, output)) throw std::runtime_error("Answer must be separate from the request");
    InstallServers(services, pem, tokens); state["tokens"] = tokens; access_setup::WriteDocument(StatePath(), state);
    access_setup::WriteDocument(output, package);
    Say(L"Ответ сохранён. Передайте этот один файл второму ПК.\nЕсли в системе уже есть другие второстепенные серверы, примените этот же ответ и на них для добавления нового клиента.\n");
}
void ApplyAnswer() {
    const auto input = ChooseFile(false, L"Выберите ответ главного ПК"); const auto package = access_setup::ReadDocument(input); access_setup::ValidatePackage(package);
    const auto& request = package.at("request"); const auto uuid = token_issuer::RequireComputerDeviceId();
    const bool target = request.at("computer_uuid") == uuid; const std::string role = target ? request.at("role").get<std::string>() : "server";
    if (target) {
        if (!fs::exists(PendingRequest())) throw std::runtime_error("Reply does not match the saved request of this computer");
        access_setup::ValidateReplyForComputer(package, access_setup::ReadDocument(PendingRequest()), uuid);
    } else Say(L"Ответ выпущен для другого ПК. Здесь будут обновлены только разрешения серверов; локальный токен клиента останется прежним.\n");
    const auto services = HasServer(role) ? SelectServices() : std::vector<Service>{}; RequireAdmin(services);
    const auto pem = package.at("public_key").get<std::string>();
    Say(L"Открытый ключ главного ПК: " + Wide(access_setup::PublicKeyFingerprint(pem)) + L"\n");
    if (!Yes(L"Файл получен от вашего главного ПК? Применить его ключ и разрешения? Работающие выбранные серверы будут перезапущены.")) throw Cancelled{};
    Preflight(services, package.at("tokens"));
    Json own = nullptr;
    if (target && HasClient(role)) {
        const auto id = request.at("identity").at("client_id");
        for (const auto& token : package.at("tokens")) if (token.at("client_id") == id) own = token;
        if (own.is_null()) throw std::runtime_error("Reply contains no token for this computer");
    }
    InstallServers(services, pem, package.at("tokens")); if (!own.is_null()) InstallLocalToken(own);
    Say(L"Доступ настроен. Закрытый ключ не переносился. В SearchClient выберите токен ПК и нажмите «Подключение».\n");
}
void CheckLocal() {
    const auto services = SelectServices();
    const bool hasToken = fs::exists(LocalToken());
    const auto token = hasToken ? access_setup::ReadDocument(LocalToken()) : Json(nullptr);
    auth_db::TokenFields fields;
    if (hasToken) {
        fields = auth_db::parseTokenFields(token);
        if (fields.device_id != token_issuer::RequireComputerDeviceId() || fields.device_type != "computer") throw std::runtime_error("Token belongs to another computer");
    } else Say(L"Локального токена нет: проверяется ключ и наличие записей клиентов на серверах.\n");
    bool all = true;
    for (const auto& service : services) {
        try {
            const auto pem = ReadBytes(auth::ResolveIssuerPublicPemPath(service.data / "auth_clients.sqlite"));
            auth::AuthClientStore store; store.open(service.data / "auth_clients.sqlite");
            access_setup::PublicKeyFingerprint(pem);
            if (!hasToken) {
                std::size_t enabled = 0; for (const auto& client : store.listClients()) if (client.enabled) ++enabled;
                Say(service.name + L" — открытый ключ читается; включено записей клиентов: " + std::to_wstring(enabled) + L".\n");
                continue;
            }
            const auto match = store.findEnabledMatch(fields.client_id, fields.client_name, fields.device_type, fields.device_id);
            if (!match || !access_setup::VerifyToken(token, pem)) throw std::runtime_error("Local token is not authorized");
            Say(service.name + L" — токен зарегистрирован, подпись подходит.\n");
        } catch (...) { all = false; Say(service.name + L" — доступ по этому токену не подтверждён.\n"); }
    }
    Say(L"Это проверка локальных разрешений. Сетевое подключение ко всем серверам проверяется кнопкой «Подключение» в SearchClient.\n");
    if (!all) throw std::runtime_error("Local access check failed");
}
std::wstring Explain(const std::string& error) {
    if (error.find("conflict") != std::string::npos) return L"Идентификатор уже занят другим компьютером или именем. Чужая запись не заменена.";
    if (error.find("administrator") != std::string::npos) return L"Для настройки серверов закройте мастер и запустите Setup-Access.bat от имени администратора под той же учётной записью Windows.";
    if (error.find("saved request") != std::string::npos) return L"Ответ не соответствует сохранённой заявке этого ПК. Выберите ответ именно на его заявку.";
    if (error.find("issuer did not") != std::string::npos) return L"Операция с токеном не завершена. Причина показана выше.";
    if (error.find("Configure this") != std::string::npos) return L"Сначала выполните пункт 1 — настройку главного ПК.";
    if (error.find("key changed") != std::string::npos) return L"Ключ главного ПК изменился. Запустите мастер снова: он предложит перевыпустить ключи и начать новую систему.";
    if (error.find("No installed") != std::string::npos) return L"Установленные серверы SearchEngine с рабочей папкой данных не найдены. Для ПК без сервера выберите роль «Только клиент».";
    if (error.find("another computer") != std::string::npos) return L"Данные принадлежат другому компьютеру.";
    if (error.find("separate") != std::string::npos) return L"Сохраните заявку или ответ отдельным файлом на флешке. Рабочие токены, ключи и настройки заменять нельзя.";
    if (error.find("signature") != std::string::npos || error.find("Untrusted") != std::string::npos) return L"Подпись токена не подходит к открытому ключу. Получите ответ от главного ПК заново.";
    if (error.find("public key") != std::string::npos) return L"В файле нет подходящего открытого ключа. Используйте ответ, сохранённый мастером на главном ПК.";
    if (error.find("no local PC token") != std::string::npos) return L"У текущего пользователя Windows ещё нет локального токена. Сначала создайте заявку и примените ответ главного ПК.";
    if (error.find("Local access check failed") != std::string::npos) return L"Не все выбранные серверы разрешают подключение по этому токену. Результаты показаны выше.";
    if (error.find("service") != std::string::npos || error.find("server") != std::string::npos) return L"Операция с выбранным сервером не завершена. Проверьте права администратора и состояние службы. Уже выполненные действия показаны выше.";
    if (error.find("write") != std::string::npos || error.find("replace") != std::string::npos || error.find("temporary") != std::string::npos) return L"Не удалось сохранить файл. Проверьте свободное место, подключение флешки и права записи в выбранную папку.";
    if (error.find("menu") != std::string::npos) return L"Такого пункта меню нет. Выберите один из показанных номеров.";
    return L"Операция не завершена. Проверьте выбранный файл, права на папки и состояние серверов. Используйте исходный файл заявки или ответа, без ручного редактирования.";
}
}
int wmain(int argc, wchar_t** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 0;
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--check-package") {
            access_setup::ValidatePackage(access_setup::ReadDocument(argv[2])); Say(L"access-package-ok\n");
        } else {
            Say(L"НАСТРОЙКА ОБЩЕГО ДОСТУПА SEARCHENGINE\n\nПрофиль Windows: " + Profile().wstring() + L"\n");
            if (ResolveAuthorityKeyConflict(false))
                Say(L"Теперь выберите пункт 1 — настройку главного ПК с новыми ключами.\n");
            Say(L"\n1 — Сделать этот ПК главным\n2 — Подготовить заявку этого ПК\n3 — Обработать заявку на главном ПК\n4 — Применить ответ главного ПК\n5 — Проверить локальные разрешения\n0 — Выход\nВыбор: ");
            const auto choice = Line();
            if (choice == L"1") ConfigureAuthority(); else if (choice == L"2") PrepareRequest();
            else if (choice == L"3") ProcessRequest(); else if (choice == L"4") ApplyAnswer();
            else if (choice == L"5") CheckLocal(); else if (choice != L"0") throw std::runtime_error("Invalid menu selection");
        }
    } catch (const Cancelled&) { Say(L"Операция отменена.\n"); code = 2; }
      catch (const std::exception& error) { Say(Explain(error.what()) + L"\n"); code = 1; }
    if (argc == 1) { Say(L"\nНажмите Enter, чтобы закрыть окно."); try { Line(); } catch (...) {} }
    if (SUCCEEDED(com)) CoUninitialize(); return code;
}
