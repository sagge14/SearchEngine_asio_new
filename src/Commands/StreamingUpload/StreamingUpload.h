#pragma once

#include "Commands/CommandResult.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace streaming_upload
{
    inline constexpr std::uint32_t kProtocolVersion = 1;
    inline constexpr std::uint64_t kMaxMetadataBytes = 16ull * 1024ull;
    inline constexpr std::size_t kChunkSize = 64u * 1024u;

    enum class Kind
    {
        Razn,
        TlgToSend
    };

    struct Metadata
    {
        std::uint32_t version{kProtocolVersion};
        std::string file_name;
        std::uint64_t file_size{0};
    };

    struct TimeParts
    {
        std::wstring monthUpper;
        std::wstring date;
        std::wstring hhmm;
    };

    struct PlannedTarget
    {
        std::filesystem::path directory;
        std::string baseName;
    };

    [[nodiscard]] bool isSafeFileComponent(std::string_view name);
    [[nodiscard]] bool isSafeBasename(std::string_view name);
    [[nodiscard]] bool isSafeOperatorComponent(std::string_view name);

    [[nodiscard]] command_execution::CommandResult parseMetadata(
        std::span<const std::uint8_t> bytes,
        Metadata& out);

    [[nodiscard]] TimeParts currentLocalTimeParts();

    [[nodiscard]] bool compositionStaysUnderDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& composed);

    [[nodiscard]] command_execution::CommandResult planRaznTarget(
        const std::filesystem::path& raznRoot,
        const Metadata& metadata,
        PlannedTarget& out);

    [[nodiscard]] command_execution::CommandResult planTlgTarget(
        const std::filesystem::path& tlgRoot,
        std::string_view operatorName,
        const Metadata& metadata,
        const TimeParts& time,
        PlannedTarget& out);

    [[nodiscard]] std::string makeReadyJson();
    [[nodiscard]] std::string makeFinalJson(std::string_view savedName);

    class StreamingUploadSink
    {
    public:
        StreamingUploadSink() = default;
        ~StreamingUploadSink();

        StreamingUploadSink(const StreamingUploadSink&) = delete;
        StreamingUploadSink& operator=(const StreamingUploadSink&) = delete;
        StreamingUploadSink(StreamingUploadSink&&) noexcept;
        StreamingUploadSink& operator=(StreamingUploadSink&&) noexcept;

        [[nodiscard]] command_execution::CommandResult prepare(
            const PlannedTarget& target,
            std::uint64_t fileSize);

        [[nodiscard]] command_execution::CommandResult writeChunk(
            std::span<const std::uint8_t> chunk);

        void abort() noexcept;

        [[nodiscard]] command_execution::CommandResult publish();

        [[nodiscard]] const std::string& savedName() const noexcept
        {
            return savedName_;
        }

        [[nodiscard]] std::uint64_t bytesWritten() const noexcept
        {
            return bytesWritten_;
        }

        [[nodiscard]] std::uint64_t advertisedSize() const noexcept
        {
            return advertisedSize_;
        }

        [[nodiscard]] const std::filesystem::path& destinationDirectory() const noexcept
        {
            return destinationDir_;
        }

        [[nodiscard]] const std::filesystem::path& publishedPath() const noexcept
        {
            return publishedPath_;
        }

        [[nodiscard]] bool isPrepared() const noexcept
        {
            return prepared_;
        }

    private:
        void closeStagingHandle() noexcept;
        void moveFrom(StreamingUploadSink& other) noexcept;

        std::filesystem::path destinationDir_{};
        std::filesystem::path stagingPath_{};
        std::filesystem::path publishedPath_{};
        std::wstring requestedFileName_{};
        std::string savedName_{};
        std::uint64_t advertisedSize_{0};
        std::uint64_t bytesWritten_{0};
        bool prepared_{false};
        bool published_{false};
#ifdef _WIN32
        HANDLE stagingHandle_{INVALID_HANDLE_VALUE};
#else
        std::FILE* stagingFile_{nullptr};
#endif
    };
}
