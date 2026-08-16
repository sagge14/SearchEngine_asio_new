#include "Auth/AuthRuntime.h"

#include "MyUtils/LogFile.h"

#include <stdexcept>

namespace auth
{
namespace {

constexpr const char* kProgramDataIssuerPublicPem =
    R"(C:\ProgramData\SearchClientTokenIssuer\keys\public.pem)";

std::filesystem::path ResolveIssuerPublicPem(
    const std::filesystem::path& db_path)
{
    const auto primary = db_path.parent_path() / "issuer-public.pem";
    if (std::filesystem::is_regular_file(primary)) {
        return primary;
    }

    const std::filesystem::path fallback(kProgramDataIssuerPublicPem);
    if (std::filesystem::is_regular_file(fallback)) {
        return fallback;
    }

    return primary;
}

} // namespace

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

        const auto primary = db_path.parent_path() / "issuer-public.pem";
        const auto public_pem = ResolveIssuerPublicPem(db_path);
        rsa_verifier_ =
            std::make_unique<RsaIdentitySignatureVerifier>(public_pem);
        if (std::filesystem::is_regular_file(public_pem)) {
            LG("Auth issuer public key path=", public_pem.string());
        } else {
            LG(
                "Auth issuer public key not found; tried ",
                primary.string(),
                " and ",
                kProgramDataIssuerPublicPem);
        }
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
