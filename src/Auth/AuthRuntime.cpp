#include "Auth/AuthRuntime.h"

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
        initialized_ = true;
    }

    void AuthRuntime::shutdown() noexcept
    {
        std::lock_guard lock(mutex_);
        store_.reset();
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
        return stub_verifier_;
    }
}
