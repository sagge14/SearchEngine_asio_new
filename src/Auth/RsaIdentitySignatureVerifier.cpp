#include "Auth/RsaIdentitySignatureVerifier.h"
#include "Auth/IdentitySigning.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <fstream>
#include <memory>
#include <vector>

namespace auth {
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

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const
    {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
    }
};

using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

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

std::vector<unsigned char> Base64Decode(std::string_view input)
{
    std::vector<unsigned char> out;
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
            return {};
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

PkeyPtr LoadPublicPemFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open issuer public key: " + path.string();
        return {};
    }
    const std::string pem(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        error = "BIO_new_mem_buf failed for issuer public key";
        return {};
    }
    EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    if (!raw) {
        error = "PEM_read_bio_PUBKEY failed for issuer public key: "
            + path.string();
        return {};
    }
    return PkeyPtr(raw);
}

bool EnsurePublicKeyLoaded(
    std::shared_ptr<void>& public_key,
    std::string& load_error,
    const std::filesystem::path& public_pem_path)
{
    if (public_key) {
        return true;
    }
    std::string error;
    auto loaded = LoadPublicPemFile(public_pem_path, error);
    if (!loaded) {
        load_error = std::move(error);
        return false;
    }
    public_key = std::shared_ptr<void>(
        loaded.release(),
        [](void* p) {
            if (p) {
                EVP_PKEY_free(static_cast<EVP_PKEY*>(p));
            }
        });
    return true;
}

} // namespace

RsaIdentitySignatureVerifier::RsaIdentitySignatureVerifier(
    std::filesystem::path public_pem_path)
    : public_pem_path_(std::move(public_pem_path))
{
}

bool RsaIdentitySignatureVerifier::publicKeyLoaded() const noexcept
{
    std::lock_guard lock(mutex_);
    return static_cast<bool>(public_key_);
}

std::optional<std::string> RsaIdentitySignatureVerifier::configurationError() const
{
    std::lock_guard lock(mutex_);
    if (EnsurePublicKeyLoaded(public_key_, load_error_, public_pem_path_)) {
        return std::nullopt;
    }
    return load_error_;
}

bool RsaIdentitySignatureVerifier::verify(
    const AuthIdentity& identity,
    std::string_view signature_base64) const
{
    if (signature_base64.empty()) {
        return false;
    }

    std::shared_ptr<void> key_holder;
    {
        std::lock_guard lock(mutex_);
        if (!EnsurePublicKeyLoaded(public_key_, load_error_, public_pem_path_)) {
            return false;
        }
        key_holder = public_key_;
    }

    auto* pkey = static_cast<EVP_PKEY*>(key_holder.get());
    const std::string message = BuildIdentitySigningMessage(
        identity.client_id,
        identity.client_name,
        identity.device_type,
        identity.device_id);
    const auto signature = Base64Decode(signature_base64);
    if (signature.empty()) {
        return false;
    }

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        return false;
    }
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey) !=
        1)
    {
        return false;
    }
    if (EVP_DigestVerifyUpdate(
            ctx.get(),
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size()) != 1)
    {
        return false;
    }
    const int ok = EVP_DigestVerifyFinal(
        ctx.get(), signature.data(), signature.size());
    return ok == 1;
}

} // namespace auth
