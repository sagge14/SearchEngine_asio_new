#pragma once

#include "CryptoStub.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace token_issuer {

struct TokenFields {
    std::string client_id;
    std::string client_name;
    std::string device_type;
    std::string device_id;
    std::string issued_at;
    nlohmann::json expires_at = nullptr;
    std::string issuer;
    std::string notes;
};

constexpr const char* kTokenFileName = "searchclient-auth-token.json";
constexpr const char* kTokenFormat = "searchclient-auth-token";
constexpr int kTokenFormatVersion = 1;

// Offline enrollment document. It is deliberately not an auth token.
constexpr const char* kRequestFileName = "searchclient-auth-request.json";
constexpr const char* kRequestFormat = "searchclient-auth-request";
constexpr int kRequestFormatVersion = 1;

nlohmann::json BuildComputerRequestDocument(const TokenFields& fields);
TokenFields ParseComputerRequestDocument(const nlohmann::json& document);
TokenFields LoadComputerRequestFile(const std::filesystem::path& path);
void WriteComputerRequestFile(
    const std::filesystem::path& path, const TokenFields& fields);

std::string NowUtcIso8601();

void ValidateTokenFields(const TokenFields& fields);

nlohmann::json BuildTokenDocument(
    const TokenFields& fields,
    const TokenSignature& signature);

std::string PreviewTokenJson(
    const TokenFields& fields,
    const TokenSignature& signature);

void WriteTokenFile(
    const std::filesystem::path& path,
    const TokenFields& fields,
    const TokenSignature& signature);

} // namespace token_issuer
