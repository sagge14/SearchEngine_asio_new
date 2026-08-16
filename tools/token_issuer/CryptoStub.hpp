#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace token_issuer {

// RSA-2048 key generation + password-encrypted private PEM (OpenSSL).
// Token signature on the stick remains stage-1 alg "none" until full rollout.

struct KeystorePaths {
    std::filesystem::path root;
    std::filesystem::path public_key;   // public.pem
    std::filesystem::path private_enc;  // private.enc.pem (encrypted PKCS#8)
    std::filesystem::path meta;         // keystore.meta.json
};

KeystorePaths ResolveKeystorePaths(const std::filesystem::path& root);

bool KeystoreExists(const KeystorePaths& paths);

// RSA-2048; writes public.pem + password-protected private.enc.pem.
void GenerateKeyPair(const KeystorePaths& paths, std::string_view password);

// Re-encrypts a PEM private key with password (AES-256-CBC PKCS#8).
void ProtectPrivateKey(
    const KeystorePaths& paths,
    std::string_view password,
    std::string_view private_pem_plaintext);

// Decrypts private.enc.pem; returns unencrypted PEM (never written to disk).
std::string UnlockPrivateKey(
    const KeystorePaths& paths,
    std::string_view password);

struct StubSignature {
    std::string alg = "none";
    std::string encoding = "base64";
    std::string value;
};

// Stage 1 wire: still alg=none (signing rollout is separate).
StubSignature SignTokenPayload(std::string_view canonical_payload_utf8);

bool VerifyTokenSignature(
    std::string_view alg,
    std::string_view signature_value,
    std::string_view canonical_payload_utf8);

std::filesystem::path DefaultKeystoreRoot();

constexpr int kRsaBits = 2048;

} // namespace token_issuer
