#include "Commands/Auth/AuthenticateCmd.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>

AuthenticateCmd::AuthenticateCmd(
    auth::AuthClientStore& store,
    const auth::IAuthSignatureVerifier& verifier)
    : store_(store)
    , verifier_(verifier)
{
}

std::vector<std::uint8_t> AuthenticateCmd::execute(
    const std::vector<std::uint8_t>& data)
{
    auto result = executeResult(data);
    if (result.failed()) {
        throw std::runtime_error(
            result.diagnostic.empty() ? "AUTHENTICATE_V1 failed"
                                      : result.diagnostic);
    }
    return std::move(result.payload);
}

command_execution::CommandResult AuthenticateCmd::executeResult(
    const std::vector<std::uint8_t>& data)
{
    using command_execution::CommandResult;
    using command_execution::ErrorCode;

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(data.begin(), data.end());
    } catch (const std::exception& ex) {
        return CommandResult::failure(
            ErrorCode::InvalidJson,
            std::string("AUTHENTICATE_V1 JSON parse failed: ") + ex.what());
    }

    if (!document.is_object()) {
        return CommandResult::failure(
            ErrorCode::InvalidJson,
            "AUTHENTICATE_V1 payload must be a JSON object");
    }

    const auto readRequiredString = [&](const char* key)
        -> std::optional<std::string> {
        if (!document.contains(key) || !document.at(key).is_string()) {
            return std::nullopt;
        }
        auto value = document.at(key).get<std::string>();
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    };

    const auto client_id = readRequiredString("client_id");
    const auto client_name = readRequiredString("client_name");
    const auto flash_serial = readRequiredString("flash_serial");
    if (!client_id || !client_name || !flash_serial) {
        return CommandResult::failure(
            ErrorCode::InvalidRequest,
            "AUTHENTICATE_V1 requires non-empty client_id, client_name, "
            "flash_serial");
    }

    std::string signature;
    if (document.contains("signature")) {
        if (!document.at("signature").is_string()) {
            return CommandResult::failure(
                ErrorCode::InvalidRequest,
                "AUTHENTICATE_V1 signature must be a string");
        }
        signature = document.at("signature").get<std::string>();
    }

    std::optional<auth::AuthClientRecord> match;
    try {
        match = store_.getClient(*client_id);
        if (!match) {
            return CommandResult::failure(
                ErrorCode::AuthFailed,
                "auth client_id not found");
        }
        if (!match->enabled) {
            return CommandResult::failure(
                ErrorCode::AuthClientDisabled,
                "auth client is disabled");
        }
        if (match->client_name != *client_name ||
            match->flash_serial != *flash_serial)
        {
            return CommandResult::failure(
                ErrorCode::AuthFailed,
                "auth identity mismatch");
        }
    } catch (const std::exception& ex) {
        return CommandResult::failure(
            ErrorCode::DatabaseQueryFailed,
            std::string("AUTHENTICATE_V1 lookup failed: ") + ex.what());
    }

    auth::AuthIdentity identity{
        *client_id,
        *client_name,
        *flash_serial};
    if (!verifier_.verify(identity, signature)) {
        return CommandResult::failure(
            ErrorCode::AuthFailed,
            "auth signature rejected");
    }

    nlohmann::json response{
        {"ok", true},
        {"client_id", *client_id},
        {"client_name", *client_name}};
    const auto encoded = response.dump();
    return CommandResult::success(
        std::vector<std::uint8_t>(encoded.begin(), encoded.end()));
}
