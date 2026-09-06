#pragma once
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>
namespace access_setup {
using Json = nlohmann::json;
Json ReadDocument(const std::filesystem::path& path);
void WriteDocument(const std::filesystem::path& path, const Json& value);
void WriteBytes(const std::filesystem::path& path, const std::string& bytes);
void ValidateRequest(const Json& request);
void ValidatePackage(const Json& package);
void ValidateReplyForComputer(const Json& package, const Json& pending, const std::string& uuid);
void InstallTrust(const std::filesystem::path& data, const std::string& pem, const Json& tokens);
std::string PublicKeyFingerprint(const std::string& pem);
bool VerifyToken(const Json& token, const std::string& pem);
void AddToken(Json& tokens, const Json& token);
Json MakePackage(const Json& request, const std::string& public_key, const Json& tokens);
bool AuthorityUsesPublicKey(const Json& state, const std::string& pem);
std::filesystem::path RetireToBackup(const std::filesystem::path& path);
}
