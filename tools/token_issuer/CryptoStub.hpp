#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace token_issuer {

// Crypto is stub; production RSA later.
// Stage 1 tokens always use signature.alg == "none".

struct KeystorePaths {
    std::filesystem::path root;
    std::filesystem::path public_key;
    std::filesystem::path private_enc;
    std::filesystem::path meta;
};

KeystorePaths ResolveKeystorePaths(const std::filesystem::path& root);

bool KeystoreExists(const KeystorePaths& paths);

// Creates placeholder key material and writes stub-protected private blob.
void GenerateKeyPair(const KeystorePaths& paths, std::string_view password);

void ProtectPrivateKey(
    const KeystorePaths& paths,
    std::string_view password,
    std::string_view private_plaintext);

// Throws if password does not match stub blob.
std::string UnlockPrivateKey(
    const KeystorePaths& paths,
    std::string_view password);

// Always returns empty value and alg "none" for stage-1 wire compatibility.
struct StubSignature {
    std::string alg = "none";
    std::string encoding = "base64";
    std::string value;
};

StubSignature SignTokenPayload(std::string_view canonical_payload_utf8);

bool VerifyTokenSignature(
    std::string_view alg,
    std::string_view signature_value,
    std::string_view canonical_payload_utf8);

std::filesystem::path DefaultKeystoreRoot();

} // namespace token_issuer
