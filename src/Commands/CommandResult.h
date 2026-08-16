#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command_execution
{
    // Internal server-side classification. These values are not part of the
    // current client/server wire protocol.
    enum class ErrorCode : std::uint32_t
    {
        PayloadTooLarge = 1,
        InvalidCommand = 2,
        CommandNotRegistered = 3,
        ServerBusy = 4,
        ServerStopping = 5,
        InvalidRequest = 6,
        InvalidJson = 7,
        InvalidBinaryPayload = 8,
        FileNotFound = 9,
        FileMetadataFailed = 10,
        FileOpenFailed = 11,
        FileReadFailed = 12,
        FileWriteFailed = 13,
        DirectoryCreateFailed = 14,
        ConfigurationError = 15,
        DatabaseOpenFailed = 16,
        DatabaseBusy = 17,
        DatabaseQueryFailed = 18,
        DatabaseSchemaFailed = 19,
        SerializationFailed = 20,
        IndexUnavailable = 21,
        IndexDataInconsistent = 22,
        IndexUpdateInProgress = 23,
        IndexUpdateFailed = 24,
        OperatorNotRegistered = 25,
        AttachmentNotFound = 26,
        MessageNotFound = 27,
        MessageSaveFailed = 28,
        MessageReadFailed = 29,
        ResponseQueueFull = 30,
        CommandExecutionFailed = 31,
        InternalError = 32,
        AuthFailed = 33,
        AuthClientDisabled = 34,
        AuthRequired = 35
    };

    [[nodiscard]] inline constexpr std::string_view toString(ErrorCode code) noexcept
    {
        switch (code)
        {
            case ErrorCode::PayloadTooLarge: return "PayloadTooLarge";
            case ErrorCode::InvalidCommand: return "InvalidCommand";
            case ErrorCode::CommandNotRegistered: return "CommandNotRegistered";
            case ErrorCode::ServerBusy: return "ServerBusy";
            case ErrorCode::ServerStopping: return "ServerStopping";
            case ErrorCode::InvalidRequest: return "InvalidRequest";
            case ErrorCode::InvalidJson: return "InvalidJson";
            case ErrorCode::InvalidBinaryPayload: return "InvalidBinaryPayload";
            case ErrorCode::FileNotFound: return "FileNotFound";
            case ErrorCode::FileMetadataFailed: return "FileMetadataFailed";
            case ErrorCode::FileOpenFailed: return "FileOpenFailed";
            case ErrorCode::FileReadFailed: return "FileReadFailed";
            case ErrorCode::FileWriteFailed: return "FileWriteFailed";
            case ErrorCode::DirectoryCreateFailed: return "DirectoryCreateFailed";
            case ErrorCode::ConfigurationError: return "ConfigurationError";
            case ErrorCode::DatabaseOpenFailed: return "DatabaseOpenFailed";
            case ErrorCode::DatabaseBusy: return "DatabaseBusy";
            case ErrorCode::DatabaseQueryFailed: return "DatabaseQueryFailed";
            case ErrorCode::DatabaseSchemaFailed: return "DatabaseSchemaFailed";
            case ErrorCode::SerializationFailed: return "SerializationFailed";
            case ErrorCode::IndexUnavailable: return "IndexUnavailable";
            case ErrorCode::IndexDataInconsistent: return "IndexDataInconsistent";
            case ErrorCode::IndexUpdateInProgress: return "IndexUpdateInProgress";
            case ErrorCode::IndexUpdateFailed: return "IndexUpdateFailed";
            case ErrorCode::OperatorNotRegistered: return "OperatorNotRegistered";
            case ErrorCode::AttachmentNotFound: return "AttachmentNotFound";
            case ErrorCode::MessageNotFound: return "MessageNotFound";
            case ErrorCode::MessageSaveFailed: return "MessageSaveFailed";
            case ErrorCode::MessageReadFailed: return "MessageReadFailed";
            case ErrorCode::ResponseQueueFull: return "ResponseQueueFull";
            case ErrorCode::CommandExecutionFailed: return "CommandExecutionFailed";
            case ErrorCode::InternalError: return "InternalError";
            case ErrorCode::AuthFailed: return "AuthFailed";
            case ErrorCode::AuthClientDisabled: return "AuthClientDisabled";
            case ErrorCode::AuthRequired: return "AuthRequired";
        }

        return "UnknownErrorCode";
    }

    struct CommandResult final
    {
        std::vector<std::uint8_t> payload{};
        std::optional<ErrorCode> error{};
        // Server log only. It must not be sent through the legacy protocol.
        std::string diagnostic{};

        CommandResult() = default;

        [[nodiscard]] bool succeeded() const noexcept
        {
            return !error.has_value();
        }

        [[nodiscard]] bool failed() const noexcept
        {
            return error.has_value();
        }

        [[nodiscard]] static CommandResult success(
            std::vector<std::uint8_t> payload = {})
        {
            CommandResult result;
            result.payload = std::move(payload);
            return result;
        }

        [[nodiscard]] static CommandResult failure(
            ErrorCode error,
            std::string diagnostic = {})
        {
            CommandResult result;
            result.error = error;
            result.diagnostic = std::move(diagnostic);
            return result;
        }
    };
}
