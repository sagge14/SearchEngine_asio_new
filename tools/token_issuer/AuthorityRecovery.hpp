#pragma once
#include "CryptoStub.hpp"
#include <nlohmann/json.hpp>

namespace token_issuer {
// The pending directory contains ONLY the newly generated key and signed state.
std::filesystem::path AuthorityRecoveryPending(const KeystorePaths& keys);
nlohmann::json ReadRecoveryDocument(const std::filesystem::path& path);
void WriteRecoveryDocument(const std::filesystem::path& path, const nlohmann::json& value);
void RecoverAuthority(const KeystorePaths& keys, const std::filesystem::path& state_path,
                      const nlohmann::json& state, std::string_view new_password);
// Administrative re-signing keeps the four identity fields unchanged.
nlohmann::json ResignToken(const nlohmann::json& token, std::string_view private_pem);
}
