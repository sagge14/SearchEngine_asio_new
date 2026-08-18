#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace auth
{
    struct AuthIdentity
    {
        std::string client_id;
        std::string client_name;
        std::string device_type;
        std::string device_id;
    };

    class IAuthSignatureVerifier
    {
    public:
        virtual ~IAuthSignatureVerifier() = default;

        [[nodiscard]] virtual bool verify(
            const AuthIdentity& identity,
            std::string_view signature) const = 0;

        // Missing/unreadable/invalid verifier material — not a signature mismatch.
        [[nodiscard]] virtual std::optional<std::string> configurationError() const
        {
            return std::nullopt;
        }
    };

    class StubAuthSignatureVerifier final : public IAuthSignatureVerifier
    {
    public:
        [[nodiscard]] bool verify(
            const AuthIdentity& /*identity*/,
            std::string_view /*signature*/) const override
        {
            return true;
        }
    };
}
