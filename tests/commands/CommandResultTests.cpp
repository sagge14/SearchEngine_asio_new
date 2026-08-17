#include <gtest/gtest.h>

#include "AsioServer/AsioServer.h"
#include "Commands/Command.h"
#include "Commands/CommandResult.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    using asio_server::COMMAND;
    using command_execution::CommandResult;
    using command_execution::ErrorCode;

    constexpr std::array<ErrorCode, 45> allErrorCodes{
        ErrorCode::PayloadTooLarge,
        ErrorCode::InvalidCommand,
        ErrorCode::CommandNotRegistered,
        ErrorCode::ServerBusy,
        ErrorCode::ServerStopping,
        ErrorCode::InvalidRequest,
        ErrorCode::InvalidJson,
        ErrorCode::InvalidBinaryPayload,
        ErrorCode::FileNotFound,
        ErrorCode::FileMetadataFailed,
        ErrorCode::FileOpenFailed,
        ErrorCode::FileReadFailed,
        ErrorCode::FileWriteFailed,
        ErrorCode::DirectoryCreateFailed,
        ErrorCode::ConfigurationError,
        ErrorCode::DatabaseOpenFailed,
        ErrorCode::DatabaseBusy,
        ErrorCode::DatabaseQueryFailed,
        ErrorCode::DatabaseSchemaFailed,
        ErrorCode::SerializationFailed,
        ErrorCode::IndexUnavailable,
        ErrorCode::IndexDataInconsistent,
        ErrorCode::IndexUpdateInProgress,
        ErrorCode::IndexUpdateFailed,
        ErrorCode::OperatorNotRegistered,
        ErrorCode::AttachmentNotFound,
        ErrorCode::MessageNotFound,
        ErrorCode::MessageSaveFailed,
        ErrorCode::MessageReadFailed,
        ErrorCode::ResponseQueueFull,
        ErrorCode::CommandExecutionFailed,
        ErrorCode::InternalError,
        ErrorCode::AuthFailed,
        ErrorCode::AuthClientDisabled,
        ErrorCode::AuthRequired,
        ErrorCode::AuthClientIdMissing,
        ErrorCode::AuthClientNameMissing,
        ErrorCode::AuthDeviceTypeMissing,
        ErrorCode::AuthDeviceIdMissing,
        ErrorCode::AuthSignatureMissing,
        ErrorCode::AuthClientIdNotFound,
        ErrorCode::AuthClientNameMismatch,
        ErrorCode::AuthDeviceTypeMismatch,
        ErrorCode::AuthDeviceIdMismatch,
        ErrorCode::AuthSignatureInvalid,
    };

    class LegacyCommand final : public Command
    {
    public:
        explicit LegacyCommand(std::vector<std::uint8_t> response)
            : response_(std::move(response))
        {
        }

        std::vector<std::uint8_t> execute(
            const std::vector<std::uint8_t>& data) override
        {
            lastRequest = data;
            ++executeCalls;
            return response_;
        }

        std::vector<std::uint8_t> lastRequest{};
        std::size_t executeCalls{};

    private:
        std::vector<std::uint8_t> response_;
    };
}

TEST(ProtocolWireOrdinals, ExistingCommandValuesRemainStable)
{
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::SOMEERROR), 0u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::SOLOREQUEST), 1u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETBINFILE), 11u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::PING), 18u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GET_SINGLE_ATACHMENT), 26u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::SERVER_BUSY_ERROR), 27u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::END_COMMAND), 28u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::ERROR_RESPONSE), 29u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::NEGOTIATE_PROTOCOL_V1), 30u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::AUTHENTICATE_V1), 31u);
    EXPECT_EQ(
        static_cast<std::uint_fast64_t>(COMMAND::SAVE_MESSAGE_TO),
        2781032419ULL);

    EXPECT_EQ(sizeof(asio_server::Header), 16u);
    EXPECT_TRUE(std::is_trivially_copyable_v<asio_server::Header>);
    EXPECT_EQ(sizeof(asio_server::search_protocol::ErrorResponseV1), 8u);
    EXPECT_EQ(sizeof(asio_server::search_protocol::ProtocolCapabilitiesV1), 8u);
}

TEST(ProtocolNegotiation, RequestAllowlistRejectsLegacyAndResponseOnlySlots)
{
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::SOLOREQUEST));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::GETBINFILE));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::USER_REGISTRY));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::NEGOTIATE_PROTOCOL_V1));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::AUTHENTICATE_V1));

    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::SOMEERROR));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::JSONREGUEST));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::ADDRESOLUTION));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::GETDOC));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::SERVER_BUSY_ERROR));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::END_COMMAND));
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::ERROR_RESPONSE));
}

TEST(ProtocolNegotiation, CapabilitiesAdvertiseTypedErrorsV1)
{
    asio_server::search_protocol::ProtocolCapabilitiesV1 capabilities{};
    capabilities.capabilities =
        asio_server::search_protocol::CAPABILITY_TYPED_ERRORS_V1 |
        asio_server::search_protocol::CAPABILITY_CLIENT_AUTH_V1;

    EXPECT_EQ(
        capabilities.version,
        asio_server::search_protocol::PROTOCOL_CAPABILITIES_VERSION);
    EXPECT_NE(
        capabilities.capabilities &
            asio_server::search_protocol::CAPABILITY_TYPED_ERRORS_V1,
        0u);
    EXPECT_NE(
        capabilities.capabilities &
            asio_server::search_protocol::CAPABILITY_CLIENT_AUTH_V1,
        0u);
}

TEST(ErrorCode, InternalValuesAreExplicitAndContiguous)
{
    for (std::size_t index = 0; index < allErrorCodes.size(); ++index)
    {
        EXPECT_EQ(
            static_cast<std::uint32_t>(allErrorCodes[index]),
            static_cast<std::uint32_t>(index + 1))
            << "ErrorCode at index " << index;
        EXPECT_NE(
            command_execution::toString(allErrorCodes[index]),
            "UnknownErrorCode")
            << "ErrorCode at index " << index;
    }
}

TEST(CommandResult, SuccessPreservesBinaryPayload)
{
    static_assert(!std::is_aggregate_v<CommandResult>);

    const std::vector<std::uint8_t> payload{0x00, 0x01, 0x7f, 0xff};

    const auto result = CommandResult::success(payload);

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.failed());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.payload, payload);
    EXPECT_TRUE(result.diagnostic.empty());

    const auto emptyResult = CommandResult::success();
    EXPECT_TRUE(emptyResult.succeeded());
    EXPECT_TRUE(emptyResult.payload.empty());
}

TEST(CommandResult, FailurePreservesCodeAndDiagnosticWithoutPayload)
{
    const auto result = CommandResult::failure(
        ErrorCode::DatabaseQueryFailed,
        "query failed");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.failed());
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, ErrorCode::DatabaseQueryFailed);
    EXPECT_TRUE(result.payload.empty());
    EXPECT_EQ(result.diagnostic, "query failed");
}

TEST(CommandResult, LegacyHandlerAdapterWrapsPayloadAsSuccess)
{
    const std::vector<std::uint8_t> request{0x10, 0x20};
    const std::vector<std::uint8_t> legacyPayload{0x00};
    LegacyCommand command(legacyPayload);
    Command& base = command;

    const auto result = base.executeResult(request);

    EXPECT_EQ(command.executeCalls, 1u);
    EXPECT_EQ(command.lastRequest, request);
    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.payload, legacyPayload);
}

TEST(LegacyMapping, EverySpecificErrorCollapsesExceptServerBusy)
{
    for (const auto error : allErrorCodes)
    {
        const auto expected = error == ErrorCode::ServerBusy
            ? COMMAND::SERVER_BUSY_ERROR
            : COMMAND::SOMEERROR;

        EXPECT_EQ(asio_server::legacyErrorCommand(error), expected)
            << "ErrorCode=" << command_execution::toString(error);
    }
}

TEST(LegacyMapping, ErrorHeaderAlwaysHasZeroPayloadSize)
{
    for (const auto error : allErrorCodes)
    {
        const auto header = asio_server::makeLegacyErrorHeader(error);
        const auto expected = error == ErrorCode::ServerBusy
            ? COMMAND::SERVER_BUSY_ERROR
            : COMMAND::SOMEERROR;

        EXPECT_EQ(header.size, 0u)
            << "ErrorCode=" << command_execution::toString(error);
        EXPECT_EQ(header.command, expected)
            << "ErrorCode=" << command_execution::toString(error);
    }
}

TEST(LegacyMapping, UnknownErrorFallsBackToSomeError)
{
    constexpr auto unknown = static_cast<ErrorCode>(
        std::numeric_limits<std::uint32_t>::max());

    EXPECT_EQ(command_execution::toString(unknown), "UnknownErrorCode");
    EXPECT_EQ(asio_server::legacyErrorCommand(unknown), COMMAND::SOMEERROR);

    const auto header = asio_server::makeLegacyErrorHeader(unknown);
    EXPECT_EQ(header.size, 0u);
    EXPECT_EQ(header.command, COMMAND::SOMEERROR);
}

TEST(TypedMapping, ErrorResponsePreservesSpecificCode)
{
    for (const auto error : allErrorCodes)
    {
        const auto response = asio_server::makeTypedErrorResponse(error);

        EXPECT_EQ(
            response.version,
            asio_server::search_protocol::ERROR_RESPONSE_VERSION);
        EXPECT_EQ(
            response.errorCode,
            static_cast<std::uint32_t>(error));
    }
}
