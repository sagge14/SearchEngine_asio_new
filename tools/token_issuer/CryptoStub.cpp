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

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(std::string_view input)
{
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < input.size()) {
        const unsigned triple =
            (static_cast<unsigned>(static_cast<unsigned char>(input[i])) << 16) |
            (static_cast<unsigned>(static_cast<unsigned char>(input[i + 1])) << 8) |
            static_cast<unsigned>(static_cast<unsigned char>(input[i + 2]));
        out.push_back(kB64[(triple >> 18) & 63]);
        out.push_back(kB64[(triple >> 12) & 63]);
        out.push_back(kB64[(triple >> 6) & 63]);
        out.push_back(kB64[triple & 63]);
        i += 3;
    }
    if (i < input.size()) {
        unsigned triple =
            static_cast<unsigned>(static_cast<unsigned char>(input[i])) << 16;
        out.push_back(kB64[(triple >> 18) & 63]);
        if (i + 1 < input.size()) {
            triple |= static_cast<unsigned>(
                          static_cast<unsigned char>(input[i + 1]))
                << 8;
            out.push_back(kB64[(triple >> 12) & 63]);
            out.push_back(kB64[(triple >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kB64[(triple >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

int B64Value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::string Base64Decode(std::string_view input)
{
    std::string out;
    int val = 0;
    int valb = -8;
    for (const char c : input) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') {
            if (c == '=') {
                break;
            }
            continue;
        }
        const int d = B64Value(c);
        if (d < 0) {
            continue;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

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
        {"signature_alg", kSignatureAlg},
        {"note",
         "RSA-2048 keystore; tokens use RS256 over identity message; "
         "server verifies"},
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

TokenSignature SignTokenPayload(
    std::string_view identity_message_utf8,
    std::string_view private_pem_plaintext)
{
    auto key = LoadPrivatePem(private_pem_plaintext, {}, false);
    if (RsaBits(key.get()) != kRsaBits) {
        throw std::runtime_error("signing key is not RSA-2048");
    }

    struct MdCtxDeleter {
        void operator()(EVP_MD_CTX* ctx) const
        {
            if (ctx) {
                EVP_MD_CTX_free(ctx);
            }
        }
    };
    std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx) {
        ThrowOpenSsl("EVP_MD_CTX_new failed");
    }
    if (EVP_DigestSignInit(
            ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1)
    {
        ThrowOpenSsl("EVP_DigestSignInit failed");
    }
    if (EVP_DigestSignUpdate(
            ctx.get(),
            reinterpret_cast<const unsigned char*>(identity_message_utf8.data()),
            identity_message_utf8.size()) != 1)
    {
        ThrowOpenSsl("EVP_DigestSignUpdate failed");
    }
    std::size_t sig_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1) {
        ThrowOpenSsl("EVP_DigestSignFinal(size) failed");
    }
    std::string signature(sig_len, '\0');
    if (EVP_DigestSignFinal(
            ctx.get(),
            reinterpret_cast<unsigned char*>(signature.data()),
            &sig_len) != 1)
    {
        ThrowOpenSsl("EVP_DigestSignFinal failed");
    }
    signature.resize(sig_len);

    TokenSignature out;
    out.alg = kSignatureAlg;
    out.encoding = "base64";
    out.value = Base64Encode(signature);
    return out;
}

bool VerifyTokenSignature(
    std::string_view identity_message_utf8,
    std::string_view signature_base64,
    std::string_view public_pem)
{
    if (signature_base64.empty() || public_pem.empty()) {
        return false;
    }

    BioPtr bio(BIO_new_mem_buf(public_pem.data(), static_cast<int>(public_pem.size())));
    if (!bio) {
        return false;
    }
    EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    if (!raw) {
        return false;
    }
    PkeyPtr key(raw);

    const std::string decoded = Base64Decode(signature_base64);
    if (decoded.empty()) {
        return false;
    }

    struct MdCtxDeleter {
        void operator()(EVP_MD_CTX* ctx) const
        {
            if (ctx) {
                EVP_MD_CTX_free(ctx);
            }
        }
    };
    std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx) {
        return false;
    }
    if (EVP_DigestVerifyInit(
            ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1)
    {
        return false;
    }
    if (EVP_DigestVerifyUpdate(
            ctx.get(),
            reinterpret_cast<const unsigned char*>(identity_message_utf8.data()),
            identity_message_utf8.size()) != 1)
    {
        return false;
    }
    return EVP_DigestVerifyFinal(
               ctx.get(),
               reinterpret_cast<const unsigned char*>(decoded.data()),
               decoded.size()) == 1;
}

void ExportPublicKey(
    const KeystorePaths& paths,
    const std::filesystem::path& destination)
{
    if (!std::filesystem::is_regular_file(paths.public_key)) {
        throw std::runtime_error(
            "public key missing: " + paths.public_key.string());
    }
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::copy_file(
        paths.public_key,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
        throw std::runtime_error(
            "cannot export public key to " + destination.string() + ": " +
            ec.message());
    }
}

} // namespace token_issuer
