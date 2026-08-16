#include "CryptoStub.hpp"

#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace token_issuer {
namespace {

struct BioDeleter {
    void operator()(BIO* bio) const
    {
        if (bio) {
            BIO_free(bio);
        }
    }
};

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const
    {
        if (key) {
            EVP_PKEY_free(key);
        }
    }
};

struct PkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const
    {
        if (ctx) {
            EVP_PKEY_CTX_free(ctx);
        }
    }
};

using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

[[noreturn]] void ThrowOpenSsl(const char* what)
{
    char buffer[256]{};
    const unsigned long err = ERR_get_error();
    if (err != 0) {
        ERR_error_string_n(err, buffer, sizeof(buffer));
        throw std::runtime_error(std::string(what) + ": " + buffer);
    }
    throw std::runtime_error(what);
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    output << text;
}

std::string BioToString(BIO* bio)
{
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    if (length < 0 || data == nullptr) {
        throw std::runtime_error("cannot read OpenSSL BIO memory");
    }
    return std::string(data, static_cast<std::size_t>(length));
}

PkeyPtr LoadPrivatePem(
    std::string_view pem,
    std::string_view password,
    bool encrypted)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        ThrowOpenSsl("BIO_new_mem_buf failed");
    }

    auto password_cb = [](char* buf, int size, int /*rwflag*/, void* userdata) -> int {
        const auto* pass = static_cast<const std::string*>(userdata);
        if (!pass || size <= 0) {
            return 0;
        }
        const int n = static_cast<int>(
            (std::min)(pass->size(), static_cast<std::size_t>(size)));
        if (n > 0) {
            memcpy(buf, pass->data(), static_cast<std::size_t>(n));
        }
        return n;
    };

    std::string pass_storage(password);
    EVP_PKEY* raw = nullptr;
    if (encrypted) {
        raw = PEM_read_bio_PrivateKey(
            bio.get(), nullptr, password_cb, &pass_storage);
    } else {
        raw = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    }
    if (!raw) {
        ThrowOpenSsl(
            encrypted ? "cannot decrypt private key (bad password or PEM)"
                      : "cannot parse private PEM");
    }
    return PkeyPtr(raw);
}

std::string PublicPemFromKey(EVP_PKEY* key)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PUBKEY(bio.get(), key) != 1) {
        ThrowOpenSsl("PEM_write_bio_PUBKEY failed");
    }
    return BioToString(bio.get());
}

std::string PrivatePemPlainFromKey(EVP_PKEY* key)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio ||
        PEM_write_bio_PrivateKey(
            bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    {
        ThrowOpenSsl("PEM_write_bio_PrivateKey (plain) failed");
    }
    return BioToString(bio.get());
}

std::string PrivatePemEncryptedFromKey(
    EVP_PKEY* key,
    std::string_view password)
{
    if (password.empty()) {
        throw std::runtime_error("password must not be empty");
    }
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) {
        ThrowOpenSsl("BIO_new failed");
    }

    // PKCS#8 encrypted with AES-256-CBC.
    if (PEM_write_bio_PKCS8PrivateKey(
            bio.get(),
            key,
            EVP_aes_256_cbc(),
            const_cast<char*>(password.data()),
            static_cast<int>(password.size()),
            nullptr,
            nullptr) != 1)
    {
        ThrowOpenSsl("PEM_write_bio_PKCS8PrivateKey failed");
    }
    return BioToString(bio.get());
}

int RsaBits(EVP_PKEY* key)
{
    return EVP_PKEY_bits(key);
}

} // namespace

KeystorePaths ResolveKeystorePaths(const std::filesystem::path& root)
{
    KeystorePaths paths;
    paths.root = root;
    paths.public_key = root / "public.pem";
    paths.private_enc = root / "private.enc.pem";
    paths.meta = root / "keystore.meta.json";
    return paths;
}

bool KeystoreExists(const KeystorePaths& paths)
{
    return std::filesystem::is_regular_file(paths.private_enc) &&
        std::filesystem::is_regular_file(paths.public_key);
}

std::filesystem::path DefaultKeystoreRoot()
{
    wchar_t* program_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&program_data, &length, L"ProgramData") == 0 &&
        program_data != nullptr)
    {
        std::filesystem::path root(program_data);
        free(program_data);
        return root / "SearchClientTokenIssuer" / "keys";
    }
    return std::filesystem::path("keys");
}

void ProtectPrivateKey(
    const KeystorePaths& paths,
    std::string_view password,
    std::string_view private_pem_plaintext)
{
    auto key = LoadPrivatePem(private_pem_plaintext, {}, false);
    if (RsaBits(key.get()) != kRsaBits) {
        throw std::runtime_error(
            "expected RSA-" + std::to_string(kRsaBits) + " private key");
    }
    WriteTextFile(
        paths.private_enc, PrivatePemEncryptedFromKey(key.get(), password));

    std::error_code ec;
    std::filesystem::remove(paths.root / "private.stub.raw", ec);
    std::filesystem::remove(paths.root / "private.pem", ec);
}

void GenerateKeyPair(const KeystorePaths& paths, std::string_view password)
{
    if (password.empty()) {
        throw std::runtime_error("password must not be empty");
    }

    std::error_code ec;
    std::filesystem::create_directories(paths.root, ec);
    if (ec) {
        throw std::runtime_error(
            "cannot create keystore directory: " + paths.root.string());
    }

    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!ctx) {
        ThrowOpenSsl("EVP_PKEY_CTX_new_id(RSA) failed");
    }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
        ThrowOpenSsl("EVP_PKEY_keygen_init failed");
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), kRsaBits) <= 0) {
        ThrowOpenSsl("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
    }

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0 || raw == nullptr) {
        ThrowOpenSsl("EVP_PKEY_keygen failed");
    }
    PkeyPtr key(raw);

    if (RsaBits(key.get()) != kRsaBits) {
        throw std::runtime_error("generated key is not RSA-2048");
    }

    WriteTextFile(paths.public_key, PublicPemFromKey(key.get()));
    WriteTextFile(
        paths.private_enc, PrivatePemEncryptedFromKey(key.get(), password));

    // Never leave an unencrypted private key on disk.
    std::filesystem::remove(paths.root / "private.pem", ec);
    std::filesystem::remove(paths.root / "private.stub.raw", ec);
    std::filesystem::remove(paths.root / "private.stub.enc", ec);
    std::filesystem::remove(paths.root / "public.stub.pem", ec);

    nlohmann::json meta = {
        {"version", 2},
        {"kty", "RSA"},
        {"bits", kRsaBits},
        {"private_protection", "PKCS8-AES-256-CBC"},
        {"public_file", "public.pem"},
        {"private_file", "private.enc.pem"},
        {"note",
         "RSA-2048 keystore; token signature.alg remains none until rollout"},
    };
    WriteTextFile(paths.meta, meta.dump(2));
}

std::string UnlockPrivateKey(
    const KeystorePaths& paths,
    std::string_view password)
{
    std::ifstream input(paths.private_enc, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read private key: " + paths.private_enc.string());
    }
    const std::string encrypted(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    auto key = LoadPrivatePem(encrypted, password, true);
    if (RsaBits(key.get()) != kRsaBits) {
        throw std::runtime_error("unlocked key is not RSA-2048");
    }
    return PrivatePemPlainFromKey(key.get());
}

StubSignature SignTokenPayload(std::string_view /*canonical_payload_utf8*/)
{
    StubSignature signature;
    signature.alg = "none";
    signature.encoding = "base64";
    signature.value.clear();
    return signature;
}

bool VerifyTokenSignature(
    std::string_view alg,
    std::string_view /*signature_value*/,
    std::string_view /*canonical_payload_utf8*/)
{
    return alg.empty() || alg == "none";
}

} // namespace token_issuer
