#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace token_issuer {

struct TokenFields {
    std::string client_id;
    std::string client_name;
    std::string flash_serial;
    std::string issued_at;
    nlohmann::json expires_at = nullptr;
    std::string issuer;
    std::string notes;
};

constexpr const char* kTokenFileName = "searchclient-auth-token.json";
constexpr const char* kTokenFormat = "searchclient-auth-token";
constexpr int kTokenFormatVersion = 1;

std::string NowUtcIso8601();

// Validates ASCII fields and required non-empty values; throws on error.
void ValidateTokenFields(const TokenFields& fields);

nlohmann::json BuildTokenDocument(const TokenFields& fields);

std::string PreviewTokenJson(const TokenFields& fields);

void WriteTokenFile(
    const std::filesystem::path& path,
    const TokenFields& fields);

} // namespace token_issuer
