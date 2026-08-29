#include "Auth/AuthRuntime.h"
#include "Auth/IssuerPublicKeyPath.h"

#include <stdexcept>

namespace auth
{
    AuthRuntime& AuthRuntime::instance()
    {
        static AuthRuntime runtime;
        return runtime;
    }

    void AuthRuntime::initialize(const std::filesystem::path& db_path)
    {
        std::lock_guard lock(mutex_);
        auto store = std::make_unique<AuthClientStore>();
        store->open(db_path);
        store_ = std::move(store);

        const auto public_pem = ResolveIssuerPublicPemPath(db_path);
        rsa_verifier_ =
            std::make_unique<RsaIdentitySignatureVerifier>(public_pem);
        initialized_ = true;
    }

    void AuthRuntime::shutdown() noexcept
    {
        std::lock_guard lock(mutex_);
        store_.reset();
        rsa_verifier_.reset();
        initialized_ = false;
    }

    bool AuthRuntime::isInitialized() const noexcept
    {
        std::lock_guard lock(mutex_);
        return initialized_ && store_ && store_->isOpen();
    }

    AuthClientStore& AuthRuntime::store()
    {
        std::lock_guard lock(mutex_);
        if (!initialized_ || !store_) {
            throw std::runtime_error("AuthRuntime is not initialized");
        }
        return *store_;
    }

    const IAuthSignatureVerifier& AuthRuntime::verifier() const
    {
        std::lock_guard lock(mutex_);
        if (!initialized_ || !rsa_verifier_) {
            throw std::runtime_error("AuthRuntime is not initialized");
        }
        return *rsa_verifier_;
    }
}
