#pragma once

#include "Auth/IAuthSignatureVerifier.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace auth
{
    // Verifies RS256 (RSA-SHA256 PKCS#1) over BuildIdentitySigningMessage.
    class RsaIdentitySignatureVerifier final : public IAuthSignatureVerifier
    {
    public:
        explicit RsaIdentitySignatureVerifier(std::filesystem::path public_pem_path);

        [[nodiscard]] bool verify(
            const AuthIdentity& identity,
            std::string_view signature_base64) const override;

        [[nodiscard]] std::optional<std::string> configurationError() const override;

        [[nodiscard]] const std::filesystem::path& publicPemPath() const noexcept
        {
            return public_pem_path_;
        }

        [[nodiscard]] bool publicKeyLoaded() const noexcept;

    private:
        std::filesystem::path public_pem_path_;
        mutable std::mutex mutex_;
        // Opaque EVP_PKEY*; loaded lazily.
        mutable std::shared_ptr<void> public_key_;
        mutable std::string load_error_;
    };
}
