#pragma once

#include "Auth/AuthClientStore.h"
#include "Auth/IAuthSignatureVerifier.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace auth
{
    class AuthRuntime
    {
    public:
        static AuthRuntime& instance();

        void initialize(const std::filesystem::path& db_path);
        void shutdown() noexcept;

        [[nodiscard]] bool isInitialized() const noexcept;
        [[nodiscard]] AuthClientStore& store();
        [[nodiscard]] const IAuthSignatureVerifier& verifier() const;

    private:
        AuthRuntime() = default;

        mutable std::mutex mutex_;
        std::unique_ptr<AuthClientStore> store_;
        StubAuthSignatureVerifier stub_verifier_{};
        bool initialized_{false};
    };
}
