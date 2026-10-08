#include "AuthorityRecovery.hpp"
#include "TokenLoader.hpp"
#include "TokenDocument.hpp"
#include "Auth/IdentitySigning.h"
#include "Auth/DeviceIdentity.h"
#include <Windows.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <fstream>
#include <set>
#include <atomic>
#include <stdexcept>

namespace token_issuer {
namespace {
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
std::string PathText(const std::filesystem::path& path) { const auto s=path.u8string(); return {s.begin(),s.end()}; }
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary); Require(bool(f), "Cannot read recovery file");
    std::string bytes(1048577, '\0'); f.read(bytes.data(), bytes.size());
    Require(!f.bad() && f.gcount() > 0 && f.gcount() <= 1048576, "Invalid recovery file size");
    bytes.resize(static_cast<std::size_t>(f.gcount())); return bytes;
}
void AtomicWrite(const std::filesystem::path& path, const std::string& bytes) {
    Require(!path.empty() && path.has_filename(), "Output file is required");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    static std::atomic<unsigned long> sequence{0};
    auto temp = path; temp += L".new-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(h != INVALID_HANDLE_VALUE, "Cannot create new recovery file");
    DWORD written{}; const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); throw std::runtime_error("Cannot publish recovery file; repeat recovery to continue");
    }
}
std::string Fingerprint(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())); Require(bio != nullptr, "Invalid public key");
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr); BIO_free(bio);
    Require(key != nullptr, "Invalid public key");
    unsigned char* der{}; const int size = i2d_PUBKEY(key, &der);
    const bool rsa = EVP_PKEY_base_id(key) == EVP_PKEY_RSA && EVP_PKEY_bits(key) == kRsaBits; EVP_PKEY_free(key);
    if (!rsa || size <= 0) { OPENSSL_free(der); throw std::runtime_error("RSA-2048 public key required"); }
    unsigned char hash[SHA256_DIGEST_LENGTH]; SHA256(der, size, hash); OPENSSL_free(der);
    const char* hex = "0123456789ABCDEF"; std::string result;
    for (auto b : hash) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
}
void ValidateState(const nlohmann::json& state) {
    Require(state.is_object() && state.contains("tokens") && state.at("tokens").is_array()
        && state.at("tokens").size() <= 256, "Invalid authority client directory");
    const auto uuid = auth::NormalizeComputerUuid(state.at("computer_uuid").get<std::string>());
    Require(uuid && *uuid == state.at("computer_uuid").get<std::string>(), "Invalid authority computer identity");
    const auto role = state.at("role").get<std::string>();
    Require(role == "server" || role == "client" || role == "client_server", "Invalid authority role");
    Require(state.at("services").is_array(), "Invalid enrolled services");
    for (const auto& s : state.at("services")) Require(s.is_string(), "Invalid enrolled service name");
    std::set<std::string> ids;
    for (const auto& token : state.at("tokens")) {
        const auto f = auth_db::parseTokenFields(token);
        TokenFields fields; fields.client_id=f.client_id; fields.client_name=f.client_name;
        fields.device_type=f.device_type; fields.device_id=f.device_id; ValidateTokenFields(fields);
        Require(ids.insert(f.client_id).second, "Duplicate client ID in authority directory");
    }
}
}
std::filesystem::path AuthorityRecoveryPending(const KeystorePaths& keys) {
    auto path = keys.root; path += L".reissue-pending"; return path;
}
nlohmann::json ReadRecoveryDocument(const std::filesystem::path& path) { return nlohmann::json::parse(Bytes(path)); }
void WriteRecoveryDocument(const std::filesystem::path& path, const nlohmann::json& value) { AtomicWrite(path, value.dump(2)); }
nlohmann::json ResignToken(const nlohmann::json& token, std::string_view private_pem) {
    const auto f = auth_db::parseTokenFields(token);
    TokenFields fields; fields.client_id=f.client_id; fields.client_name=f.client_name;
    fields.device_type=f.device_type; fields.device_id=f.device_id;
    const auto signature = SignTokenPayload(auth::BuildIdentitySigningMessage(
        fields.client_id, fields.client_name, fields.device_type, fields.device_id), private_pem);
    // Only the canonical public identity and signature are transported.
    const auto full = BuildTokenDocument(fields, signature);
    return {{"format", kTokenFormat}, {"format_version", 1}, {"client_id", full.at("client_id")},
        {"client_name", full.at("client_name")}, {"device_type", full.at("device_type")},
        {"device_id", full.at("device_id")}, {"signature", full.at("signature")}};
}
void RecoverAuthority(const KeystorePaths& keys, const std::filesystem::path& state_path,
                      const nlohmann::json& state, std::string_view password) {
    namespace fs = std::filesystem;
    Require(!keys.root.empty() && keys.root.has_filename() && !state_path.empty(), "Recovery paths required");
    const auto root = fs::weakly_canonical(keys.root); const auto target = fs::weakly_canonical(state_path);
    const auto pending = AuthorityRecoveryPending(keys);
    Require(fs::weakly_canonical(pending) == fs::weakly_canonical(pending.parent_path()) / pending.filename(),
        "Pending key directory must not redirect outside its parent");
    const auto relative = target.lexically_relative(fs::weakly_canonical(pending));
    const auto inKeys = target.lexically_relative(root);
    Require(!relative.empty() && *relative.begin() == L".." && !inKeys.empty() && *inKeys.begin() == L"..",
        "Authority state must be outside key directories");
    fs::create_directories(root.parent_path());
    auto lockPath = keys.root; lockPath += L".reissue-lock";
    HANDLE handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "Another recovery is already running");
    struct Lock { HANDLE handle; ~Lock() { CloseHandle(handle); } } lock{handle};
    const auto fresh = ResolveKeystorePaths(pending / "keys");
    const auto ready = pending / "ready.json";
    const auto preparing = pending / "preparing.json";
    if (!fs::exists(ready)) {
        ValidateState(state); Require(!password.empty(), "New password must not be empty");
        // Incomplete preparation has not changed any active key or state file.
        if (fs::exists(pending)) {
            Require(fs::is_directory(pending), "Pending recovery path is not a directory");
            if (!fs::is_empty(pending)) {
                Require(fs::is_regular_file(preparing) && ReadRecoveryDocument(preparing).at("state_path").get<std::string>()==PathText(target),
                    "Pending directory is not owned by this recovery; nothing was deleted");
            }
            fs::remove_all(pending);
        }
        try {
            WriteRecoveryDocument(preparing, {{"state_path", PathText(target)}});
            GenerateKeyPair(fresh, password);
            const auto private_key = UnlockPrivateKey(fresh, password);
            const auto pem = Bytes(fresh.public_key);
            auto next = state;
            for (auto& token : next["tokens"]) token = ResignToken(token, private_key);
            next["public_key_fingerprint"] = Fingerprint(pem);
            next["reissue_prepared"] = true;
            WriteRecoveryDocument(pending / "state.json", next);
            // Commit marker is last: afterwards retry always uses this SAME new key.
            WriteRecoveryDocument(ready, {{"state_path", PathText(target)}});
        } catch (...) { if (!fs::exists(ready)) fs::remove_all(pending); throw; }
    }
    const auto marker = ReadRecoveryDocument(ready);
    Require(marker.at("state_path").get<std::string>() == PathText(target), "Pending recovery belongs to another authority state");
    const auto next = ReadRecoveryDocument(pending / "state.json"); ValidateState(next);
    const auto private_key = UnlockPrivateKey(fresh, password); const auto pem = Bytes(fresh.public_key);
    const auto proof = SignTokenPayload("searchengine-recovery-key-check", private_key);
    Require(VerifyTokenSignature("searchengine-recovery-key-check", proof.value, pem), "New key pair does not match");
    Require(next.at("public_key_fingerprint") == Fingerprint(pem), "New authority state/key mismatch");
    for (const auto& token : next.at("tokens")) {
        const auto f = auth_db::parseTokenFields(token);
        Require(VerifyTokenSignature(auth::BuildIdentitySigningMessage(f.client_id,f.client_name,f.device_type,f.device_id),
            token.at("signature").at("value").get<std::string>(), pem), "Invalid new token signature");
    }
    // Each file is replaced atomically. No old key is copied or renamed aside.
    AtomicWrite(keys.public_key, pem);
    AtomicWrite(keys.private_enc, Bytes(fresh.private_enc));
    AtomicWrite(keys.meta, Bytes(fresh.meta));
    for (const auto* name : {"private.pem", "private.stub.raw", "private.stub.enc", "public.stub.pem"}) fs::remove(keys.root / name);
    WriteRecoveryDocument(target, next);
    fs::remove_all(pending);
}
}
