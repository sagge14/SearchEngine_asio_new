#include <gtest/gtest.h>

#include "AsioServer/AsioServer.h"
#include "Auth/AuthClientStore.h"
#include "Auth/IAuthSignatureVerifier.h"
#include "Commands/Auth/AuthenticateCmd.h"
#include "Commands/CommandResult.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using asio_server::COMMAND;
    using command_execution::ErrorCode;

    class AcceptingVerifier final : public auth::IAuthSignatureVerifier
    {
    public:
        [[nodiscard]] bool verify(
            const auth::AuthIdentity&,
            std::string_view) const override
        {
            return true;
        }
    };

    class RejectingVerifier final : public auth::IAuthSignatureVerifier
    {
    public:
        [[nodiscard]] bool verify(
            const auth::AuthIdentity&,
            std::string_view) const override
        {
            return false;
        }
    };

    [[nodiscard]] std::vector<std::uint8_t> jsonPayload(const nlohmann::json& document)
    {
        const auto encoded = document.dump();
        return {encoded.begin(), encoded.end()};
    }

    [[nodiscard]] std::vector<std::uint8_t> authenticatePayload(
        std::optional<std::string_view> client_id,
        std::optional<std::string_view> client_name,
        std::optional<std::string_view> device_type,
        std::optional<std::string_view> device_id,
        std::optional<std::string_view> signature = "test-signature")
    {
        nlohmann::json document = nlohmann::json::object();
        if (client_id) {
            document["client_id"] = *client_id;
        }
        if (client_name) {
            document["client_name"] = *client_name;
        }
        if (device_type) {
            document["device_type"] = *device_type;
        }
        if (device_id) {
            document["device_id"] = *device_id;
        }
        if (signature) {
            document["signature"] = *signature;
        }
        return jsonPayload(document);
    }

    class TempAuthDb
    {
    public:
        TempAuthDb()
            : path_(std::filesystem::temp_directory_path() /
                    ("se_session_auth_" +
                     std::to_string(
                         static_cast<unsigned long long>(
                             reinterpret_cast<std::uintptr_t>(this))) +
                     ".sqlite"))
        {
            std::filesystem::remove(path_);
            store_.open(path_);
        }

        ~TempAuthDb()
        {
            store_.close();
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        auth::AuthClientStore& store() noexcept
        {
            return store_;
        }

    private:
        std::filesystem::path path_;
        auth::AuthClientStore store_;
    };

    void expectAuthFailure(
        const command_execution::CommandResult& result,
        ErrorCode expected)
    {
        EXPECT_TRUE(result.failed());
        ASSERT_TRUE(result.error.has_value());
        EXPECT_EQ(*result.error, expected);
        EXPECT_TRUE(result.payload.empty());
    }
}

TEST(SessionAuthGate, UnauthenticatedDataCommandIsRejectedAndCloses)
{
    // TEST 1: SOLOREQUEST without auth must not reach Command handlers.
    const auto gate = asio_server::evaluateSessionCommandGate(
        COMMAND::SOLOREQUEST,
        false);

    EXPECT_FALSE(gate.allow_execute);
    EXPECT_TRUE(gate.close_after_auth_required);
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::SOLOREQUEST));
}

TEST(SessionAuthGate, NonAdminUserRegistryPayloadIsRejected)
{
    // TEST 2: arbitrary legacy usernames must not authorize.
    const auto loopback = boost::asio::ip::make_address("127.0.0.1");
    for (const auto* payload : {
             "old_user",
             "vasya",
             "",
             "ADMIN",
             "admin123",
             "admin ",
             " admin"})
    {
        EXPECT_FALSE(asio_server::isLegacyAdminUserRegistryPayload(payload))
            << payload;
        EXPECT_FALSE(
            asio_server::mayAuthorizeLegacyAdmin(payload, true, loopback))
            << payload;
    }
}

TEST(SessionAuthGate, AdminUserRegistryRequiresIpv4LocalhostPeer)
{
    // TEST 3: "admin" + exactly 127.0.0.1 authorizes; other peers fail closed.
    EXPECT_TRUE(asio_server::isLegacyAdminUserRegistryPayload("admin"));

    const auto peer127 = boost::asio::ip::make_address("127.0.0.1");
    EXPECT_TRUE(asio_server::isLegacyAdminPeerAddress(peer127));
    EXPECT_TRUE(asio_server::mayAuthorizeLegacyAdmin("admin", true, peer127));

    for (const auto* addressText : {
             "192.168.1.10",
             "127.0.0.2",
             "127.0.0.0",
             "10.0.0.1",
             "::1",
             "0.0.0.0"})
    {
        const auto peer = boost::asio::ip::make_address(addressText);
        EXPECT_FALSE(asio_server::isLegacyAdminPeerAddress(peer)) << addressText;
        EXPECT_FALSE(
            asio_server::mayAuthorizeLegacyAdmin("admin", true, peer))
            << addressText;
    }

    // Fail closed when remote_endpoint lookup did not succeed.
    EXPECT_FALSE(
        asio_server::mayAuthorizeLegacyAdmin("admin", false, peer127));

    const auto afterAdmin = asio_server::evaluateSessionCommandGate(
        COMMAND::SOLOREQUEST,
        true);
    EXPECT_TRUE(afterAdmin.allow_execute);
}

TEST(SessionAuthGate, LocalhostOrdinaryUserRegistryIsRejected)
{
    const auto peer127 = boost::asio::ip::make_address("127.0.0.1");
    EXPECT_FALSE(
        asio_server::mayAuthorizeLegacyAdmin("ordinary_user", true, peer127));
    EXPECT_FALSE(
        asio_server::mayAuthorizeLegacyAdmin("vasya", true, peer127));
}

TEST(SessionAuthGate, AuthenticateUnknownClientFailsClosed)
{
    // TEST 4: missing client_id in empty AuthClientStore.
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    const auto result = command.executeResult(
        authenticatePayload("missing-id", "client", "usb", "USB-SERIAL-1"));

    expectAuthFailure(result, ErrorCode::AuthClientIdNotFound);
}

TEST(SessionAuthGate, SuccessfulAuthenticateAllowsDataGateWithoutLocalhost)
{
    // TEST 5: AUTHENTICATE_V1 success is independent of TCP peer address.
    // Legacy admin is localhost-only; AUTHENTICATE_V1 must remain available
    // to ordinary remote network clients.
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    const auto result = command.executeResult(
        authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1"));

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.error.has_value());

    // Session sets authenticated_ after successful AUTHENTICATE_V1 regardless
    // of peer (remote LAN clients are expected).
    const auto remotePeer = boost::asio::ip::make_address("192.168.1.10");
    EXPECT_FALSE(asio_server::isLegacyAdminPeerAddress(remotePeer));
    EXPECT_FALSE(
        asio_server::mayAuthorizeLegacyAdmin("admin", true, remotePeer));

    const auto gate = asio_server::evaluateSessionCommandGate(
        COMMAND::GETSQLJSONANSWEAR,
        true);
    EXPECT_TRUE(gate.allow_execute);

    const auto beforeAuth = asio_server::evaluateSessionCommandGate(
        COMMAND::GETSQLJSONANSWEAR,
        false);
    EXPECT_FALSE(beforeAuth.allow_execute);
}

TEST(SessionAuthGate, BootstrapCommandsAllowedWithoutAuthentication)
{
    // TEST 6
    for (const auto command : {
             COMMAND::PING,
             COMMAND::NEGOTIATE_PROTOCOL_V1,
             COMMAND::USER_REGISTRY,
             COMMAND::AUTHENTICATE_V1})
    {
        EXPECT_TRUE(asio_server::isSessionBootstrapCommand(command));
        const auto gate = asio_server::evaluateSessionCommandGate(command, false);
        EXPECT_TRUE(gate.allow_execute) << asio_server::getTextCommand(command);
    }
}

TEST(SessionAuthGate, LegacySaveMessageRequiresAuthentication)
{
    // TEST 7: SAVE_MESSAGE_TO (including after trustCommand rewrite) is data.
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::SAVE_MESSAGE_TO));

    const auto gate = asio_server::evaluateSessionCommandGate(
        COMMAND::SAVE_MESSAGE_TO,
        false);
    EXPECT_FALSE(gate.allow_execute);
    EXPECT_TRUE(gate.close_after_auth_required);
}

TEST(SessionAuthGate, DataCommandsRemainBlockedUntilAuthenticated)
{
    const COMMAND dataCommands[] = {
        COMMAND::SOLOREQUEST,
        COMMAND::FILETEXT,
        COMMAND::GETSQLJSONANSWEAR,
        COMMAND::GETBINFILE,
        COMMAND::GET_VH_TELEGI_FROM_SQL,
        COMMAND::GET_ISH_TELEGI_FROM_SQL,
        COMMAND::GET_OPIS_BASE,
        COMMAND::GET_ATTACHMENTS,
        COMMAND::GET_ISH_PDTV,
        COMMAND::GET_TELEGA_ATACHMENTS,
        COMMAND::GET_SINGLE_ATACHMENT,
        COMMAND::GET_TELEGA_TEXT,
        COMMAND::GET_ISH_PDTV_TEXT,
        COMMAND::UPLOAD_TLG_TO_SEND_V1,
        COMMAND::UPLOAD_RAZN_V1,
        COMMAND::LOAD_TLG_TO_SEND,
        COMMAND::LOAD_RAZN,
        COMMAND::GET_MESSAGE,
        COMMAND::SAVE_MESSAGE_TO,
        COMMAND::START_UPDATE_BASE,
        COMMAND::GET_VH_TELEGA_WAY,
        COMMAND::GET_ISH_TELEGA_WAY,
    };

    for (const auto command : dataCommands)
    {
        const auto before = asio_server::evaluateSessionCommandGate(command, false);
        EXPECT_FALSE(before.allow_execute) << asio_server::getTextCommand(command);
        EXPECT_TRUE(before.close_after_auth_required);

        const auto after = asio_server::evaluateSessionCommandGate(command, true);
        EXPECT_TRUE(after.allow_execute) << asio_server::getTextCommand(command);
    }
}

TEST(SessionAuthGate, AuthenticateRejectsDisabledClient)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", false);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    const auto result = command.executeResult(
        authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1"));

    expectAuthFailure(result, ErrorCode::AuthClientDisabled);
}

TEST(AuthenticateV1Errors, MissingClientId)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload(std::nullopt, "desk-a", "usb", "USB-SERIAL-1")),
        ErrorCode::AuthClientIdMissing);
}

TEST(AuthenticateV1Errors, MissingClientName)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", std::nullopt, "usb", "USB-SERIAL-1")),
        ErrorCode::AuthClientNameMissing);
}

TEST(AuthenticateV1Errors, MissingDeviceType)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", std::nullopt, "USB-SERIAL-1")),
        ErrorCode::AuthDeviceTypeMissing);
}

TEST(AuthenticateV1Errors, MissingDeviceId)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", "usb", std::nullopt)),
        ErrorCode::AuthDeviceIdMissing);
}

TEST(AuthenticateV1Errors, MissingSignature)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload(
                "client-1",
                "desk-a",
                "usb",
                "USB-SERIAL-1",
                std::nullopt)),
        ErrorCode::AuthSignatureMissing);
}

TEST(AuthenticateV1Errors, EmptySignature)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1", "")),
        ErrorCode::AuthSignatureMissing);
}

TEST(AuthenticateV1Errors, UnknownClientId)
{
    TempAuthDb db;
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("missing-id", "desk-a", "usb", "USB-SERIAL-1")),
        ErrorCode::AuthClientIdNotFound);
}

TEST(AuthenticateV1Errors, DisabledClient)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", false);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1")),
        ErrorCode::AuthClientDisabled);
}

TEST(AuthenticateV1Errors, ClientNameMismatch)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "other-name", "usb", "USB-SERIAL-1")),
        ErrorCode::AuthClientNameMismatch);
}

TEST(AuthenticateV1Errors, DeviceTypeMismatch)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload(
                "client-1",
                "desk-a",
                "computer",
                "USB-SERIAL-1")),
        ErrorCode::AuthDeviceTypeMismatch);
}

TEST(AuthenticateV1Errors, DeviceIdMismatch)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", "usb", "OTHER-SERIAL")),
        ErrorCode::AuthDeviceIdMismatch);
}

TEST(AuthenticateV1Errors, InvalidSignature)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    RejectingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    expectAuthFailure(
        command.executeResult(
            authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1")),
        ErrorCode::AuthSignatureInvalid);
}

TEST(AuthenticateV1Errors, ValidUsbIdentityAndSignatureSucceeds)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    const auto result = command.executeResult(
        authenticatePayload("client-1", "desk-a", "usb", "USB-SERIAL-1"));

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_FALSE(result.payload.empty());
}

TEST(AuthenticateV1Errors, ValidComputerIdentityAndSignatureSucceeds)
{
    TempAuthDb db;
    db.store().upsertClient(
        "pc-1",
        "desk-pc",
        "computer",
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890",
        true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    const auto result = command.executeResult(
        authenticatePayload(
            "pc-1",
            "desk-pc",
            "computer",
            "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"));

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_FALSE(result.payload.empty());
}

TEST(AuthenticateV1Errors, LegacyFlashSerialOnlyRequestFails)
{
    TempAuthDb db;
    db.store().upsertClient("client-1", "desk-a", "usb", "USB-SERIAL-1", true);
    AcceptingVerifier verifier;
    AuthenticateCmd command(db.store(), verifier);

    nlohmann::json document = {
        {"client_id", "client-1"},
        {"client_name", "desk-a"},
        {"flash_serial", "USB-SERIAL-1"},
        {"signature", "test-signature"}};
    expectAuthFailure(
        command.executeResult(jsonPayload(document)),
        ErrorCode::AuthDeviceTypeMissing);
}

TEST(AuthenticateV1Errors, ExplicitWireNumericValues)
{
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthFailed), 33u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthClientDisabled), 34u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthRequired), 35u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthClientIdMissing), 36u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthClientNameMissing), 37u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthDeviceTypeMissing), 38u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthDeviceIdMissing), 39u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthSignatureMissing), 40u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthClientIdNotFound), 41u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthClientNameMismatch), 42u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthDeviceTypeMismatch), 43u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthDeviceIdMismatch), 44u);
    EXPECT_EQ(static_cast<std::uint32_t>(ErrorCode::AuthSignatureInvalid), 45u);
}

TEST(AuthenticateV1Errors, TypedErrorResponsePreservesAuthCodes)
{
    for (const auto error : {
             ErrorCode::AuthClientIdMissing,
             ErrorCode::AuthClientNameMissing,
             ErrorCode::AuthDeviceTypeMissing,
             ErrorCode::AuthDeviceIdMissing,
             ErrorCode::AuthSignatureMissing,
             ErrorCode::AuthClientIdNotFound,
             ErrorCode::AuthClientDisabled,
             ErrorCode::AuthClientNameMismatch,
             ErrorCode::AuthDeviceTypeMismatch,
             ErrorCode::AuthDeviceIdMismatch,
             ErrorCode::AuthSignatureInvalid})
    {
        const auto response = asio_server::makeTypedErrorResponse(error);
        EXPECT_EQ(
            response.errorCode,
            static_cast<std::uint32_t>(error));
        EXPECT_EQ(
            sizeof(response),
            sizeof(asio_server::search_protocol::ErrorResponseV1));
    }
}
