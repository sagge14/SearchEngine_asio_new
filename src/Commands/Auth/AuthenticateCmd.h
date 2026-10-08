#pragma once

#include "Auth/AuthClientStore.h"
#include "Auth/IAuthSignatureVerifier.h"
#include "Commands/Command.h"

class AuthenticateCmd : public Command
{
public:
    AuthenticateCmd(
        auth::AuthClientStore& store,
        const auth::IAuthSignatureVerifier& verifier);

    std::vector<std::uint8_t> execute(
        const std::vector<std::uint8_t>& data) override;

    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<std::uint8_t>& data) override;

private:
    auth::AuthClientStore& store_;
    const auth::IAuthSignatureVerifier& verifier_;
};
