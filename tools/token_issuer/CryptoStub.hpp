#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace token_issuer {

// RSA-2048 keystore + RS256 sign/verify of identity message.
// Crypto verification of tokens is performed on the server only.

struct KeystorePaths {
    std::filesystem::path root;
    std::filesystem::path public_key;   // public.pem
    std::filesystem::path private_enc;  // private.enc.pem
    std::filesystem::path meta;
};

KeystorePaths ResolveKeystorePaths(const std::filesystem::path& root);

bool KeystoreExists(const KeystorePaths& paths);

void GenerateKeyPair(const KeystorePaths& paths, std::string_view password);

void ProtectPrivateKey(
    const KeystorePaths& paths,
    std::string_view password,
    std::string_view private_pem_plaintext);

std::string UnlockPrivateKey(
    const KeystorePaths& paths,
    std::string_view password);

struct TokenSignature {
    std::string alg = "RS256";
    std::string encoding = "base64";
    std::string value;
};

TokenSignature SignTokenPayload(
    std::string_view identity_message_utf8,
    std::string_view private_pem_plaintext);

bool VerifyTokenSignature(
    std::string_view identity_message_utf8,
    std::string_view signature_base64,
    std::string_view public_pem);

void ExportPublicKey(
    const KeystorePaths& paths,
    const std::filesystem::path& destination);

std::filesystem::path DefaultKeystoreRoot();

constexpr int kRsaBits = 2048;
constexpr const char* kSignatureAlg = "RS256";

} // namespace token_issuer
