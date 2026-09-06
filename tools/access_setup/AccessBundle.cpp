#include "AccessBundle.hpp"
#include "TokenLoader.hpp"
#include "TokenDocument.hpp"
#include "CryptoStub.hpp"
#include "Auth/IdentitySigning.h"
#include "Auth/DeviceIdentity.h"
#include "Auth/AuthClientStore.h"
#include <Windows.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <fstream>
#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>

namespace access_setup {
namespace {
void Require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
void CheckHeader(const Json& value, const char* format) {
    Require(value.is_object() && value.value("format", "") == format &&
            value.contains("version") && value["version"].is_number_integer() && value["version"] == 1,
            "Unsupported access file format");
}
void CheckRole(const std::string& role) {
    Require(role == "client" || role == "server" || role == "client_server", "Invalid computer role");
}
}
Json ReadDocument(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Cannot open access file");
    std::string bytes(1024 * 1024 + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(!input.bad() && input.gcount() > 0 && input.gcount() <= 1024 * 1024, "Access file must be 1..1048576 bytes");
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return Json::parse(bytes);
}
void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    Require(!path.empty() && path.has_filename(), "Output file is required");
    Require(bytes.size() <= (std::numeric_limits<DWORD>::max)(), "Output file is too large");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    static std::atomic<unsigned long> sequence{0};
    auto temporary = path; temporary += L".access-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp";
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(output != INVALID_HANDLE_VALUE, "Cannot create temporary access file");
    DWORD written{};
    const bool saved = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(output);
    CloseHandle(output);
    if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot write or replace access file");
    }
}
void WriteDocument(const std::filesystem::path& path, const Json& value) { WriteBytes(path, value.dump(2)); }
std::string PublicKeyFingerprint(const std::string& pem) {
    Require(pem.size() <= 16384 && pem.find("PRIVATE KEY") == std::string::npos, "Only a public key may be transferred");
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    Require(bio != nullptr, "Invalid public key");
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    Require(key != nullptr, "Invalid public key");
    unsigned char* der = nullptr;
    const int size = i2d_PUBKEY(key, &der);
    const bool rsa = EVP_PKEY_base_id(key) == EVP_PKEY_RSA && EVP_PKEY_bits(key) >= 2048;
    EVP_PKEY_free(key);
    if (size <= 0 || !rsa) { OPENSSL_free(der); throw std::runtime_error("RSA-2048 public key is required"); }
    unsigned char hash[SHA256_DIGEST_LENGTH]; SHA256(der, size, hash); OPENSSL_free(der);
    const char* hex = "0123456789ABCDEF"; std::string result;
    for (auto b : hash) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
bool VerifyToken(const Json& token, const std::string& pem) {
    const auto f = auth_db::parseTokenFields(token);
    return token_issuer::VerifyTokenSignature(auth::BuildIdentitySigningMessage(
        f.client_id, f.client_name, f.device_type, f.device_id), token.at("signature").at("value").get<std::string>(), pem);
}
void ValidateRequest(const Json& request) {
    CheckHeader(request, "searchengine-access-request");
    Require(request.size() == 6, "Unexpected request fields");
    const auto uuid = request.at("computer_uuid").get<std::string>();
    const auto normalized = auth::NormalizeComputerUuid(uuid);
    Require(normalized && *normalized == uuid, "Invalid requesting computer UUID");
    const auto request_id = request.at("request_id").get<std::string>();
    Require(request_id.size() == 36 && auth::NormalizeComputerUuid(request_id).has_value(), "Invalid request identifier");
    const auto role = request.at("role").get<std::string>(); CheckRole(role);
    if (role == "server") Require(request.at("identity").is_null(), "Server-only request must not create a client identity");
    else {
        const auto identity = token_issuer::ParseComputerRequestDocument(request.at("identity"));
        Require(identity.device_id == uuid, "Request identity does not match the requesting computer");
    }
}
Json TransferToken(const Json& token) {
    const auto fields = auth_db::parseTokenFields(token);
    // Export only the signed identity and signature, never arbitrary metadata
    // from an existing local file or the issuer's private working files.
    return {{"format", "searchclient-auth-token"}, {"format_version", 1},
        {"client_id", fields.client_id}, {"client_name", fields.client_name},
        {"device_type", fields.device_type}, {"device_id", fields.device_id},
        {"signature", {{"alg", "RS256"}, {"encoding", "base64"}, {"value", token.at("signature").at("value")}}}};
}
void AddToken(Json& tokens, const Json& token) {
    const auto f = auth_db::parseTokenFields(token);
    Require(tokens.is_array(), "Invalid client directory");
    for (auto& existing : tokens) {
        const auto old = auth_db::parseTokenFields(existing);
        if (old.client_id != f.client_id) continue;
        Require(old.client_name == f.client_name && old.device_type == f.device_type && old.device_id == f.device_id,
                "Client ID conflicts with another identity");
        existing = TransferToken(token); return;
    }
    Require(tokens.size() < 256, "Client directory limit reached"); tokens.push_back(TransferToken(token));
}
void ValidatePackage(const Json& package) {
    CheckHeader(package, "searchengine-access-package");
    Require(package.size() == 5, "Unexpected package fields");
    ValidateRequest(package.at("request"));
    const auto pem = package.at("public_key").get<std::string>(); PublicKeyFingerprint(pem);
    const auto& tokens = package.at("tokens");
    Require(tokens.is_array() && tokens.size() <= 256, "Invalid client directory");
    std::set<std::string> ids; bool targetFound = package.at("request").at("role") == "server";
    for (const auto& token : tokens) {
        Require(token == TransferToken(token), "Unexpected token fields in access package");
        const auto f = auth_db::parseTokenFields(token);
        Require(ids.insert(f.client_id).second, "Duplicate client ID in access package");
        Require(VerifyToken(token, pem), "Token signature is not trusted by this package");
        if (!package.at("request").at("identity").is_null()) {
            const auto requested = token_issuer::ParseComputerRequestDocument(package.at("request").at("identity"));
            if (f.client_id == requested.client_id && f.client_name == requested.client_name &&
                f.device_type == requested.device_type && f.device_id == requested.device_id) targetFound = true;
        }
    }
    Require(targetFound, "Reply does not contain the requested computer token");
}
void ValidateReplyForComputer(const Json& package, const Json& pending, const std::string& uuid) {
    ValidatePackage(package); ValidateRequest(pending);
    Require(package.at("request") == pending && pending.at("computer_uuid") == uuid,
            "Reply does not match the saved request of this computer");
}
void InstallTrust(const std::filesystem::path& data, const std::string& pem, const Json& tokens) {
    PublicKeyFingerprint(pem);
    for (const auto& token : tokens) Require(VerifyToken(token, pem), "Untrusted token");
    auth::AuthClientStore store; store.open(data / "auth_clients.sqlite");
    const auto key = data / "issuer-public.pem";
    const bool existed = std::filesystem::is_regular_file(key); std::string original;
    if (existed) { std::ifstream input(key, std::ios::binary); original.assign(std::istreambuf_iterator<char>(input), {}); }
    bool published = false;
    store.beginTransaction();
    try {
        for (const auto& token : tokens) { const auto f = auth_db::parseTokenFields(token);
            store.upsertClient(f.client_id, f.client_name, f.device_type, f.device_id, true, f.signature_meta, true, true); }
        if (existed && original != pem) WriteBytes(data / "issuer-public.before-access.pem", original);
        WriteBytes(key, pem); published = true;
        store.commitTransaction();
    } catch (...) {
        store.rollbackTransaction();
        if (published) {
            if (existed) WriteBytes(key, original);
            else std::filesystem::remove(key);
        }
        throw;
    }
}
bool AuthorityUsesPublicKey(const Json& state, const std::string& pem) {
    Require(state.is_object() && state.contains("public_key_fingerprint") &&
            state.at("public_key_fingerprint").is_string(), "Invalid authority state");
    return state.at("public_key_fingerprint").get<std::string>() == PublicKeyFingerprint(pem);
}
std::filesystem::path RetireToBackup(const std::filesystem::path& path) {
    Require(!path.empty() && path.has_filename(), "Backup path is required");
    if (!std::filesystem::exists(path)) return {};
    auto backup = path;
    backup += L".before-reissue";
    if (std::filesystem::exists(backup)) {
        backup += L"-";
        backup += std::to_wstring(GetTickCount64());
    }
    std::error_code error;
    std::filesystem::rename(path, backup, error);
    Require(!error && std::filesystem::exists(backup) && !std::filesystem::exists(path),
            "Cannot retire existing access files");
    return backup;
}
Json MakePackage(const Json& request, const std::string& public_key, const Json& tokens) {
    Require(tokens.is_array(), "Invalid client directory");
    Json exported = Json::array(); for (const auto& token : tokens) exported.push_back(TransferToken(token));
    Json result{{"format", "searchengine-access-package"}, {"version", 1},
                {"request", request}, {"public_key", public_key}, {"tokens", exported}};
    ValidatePackage(result); return result;
}
}
