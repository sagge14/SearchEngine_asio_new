#pragma once

#include "Auth/AuthClientStore.h"
#include "Auth/IAuthSignatureVerifier.h"
#include "Auth/RsaIdentitySignatureVerifier.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace auth
{
    class AuthRuntime
    {
    public:
        static AuthRuntime& instance();

        // db_path is .../auth_clients.sqlite; public key is sibling issuer-public.pem,
        // with fallback to ProgramData\SearchClientTokenIssuer\keys\public.pem
        void initialize(const std::filesystem::path& db_path);
        void shutdown() noexcept;

        [[nodiscard]] bool isInitialized() const noexcept;
        [[nodiscard]] AuthClientStore& store();
        [[nodiscard]] const IAuthSignatureVerifier& verifier() const;

    private:
        AuthRuntime() = default;

        mutable std::mutex mutex_;
        std::unique_ptr<AuthClientStore> store_;
        std::unique_ptr<RsaIdentitySignatureVerifier> rsa_verifier_;
        bool initialized_{false};
    };
}
