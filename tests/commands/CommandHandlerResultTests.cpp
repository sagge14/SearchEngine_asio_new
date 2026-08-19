#include "Commands/GetFile/GetFileCmd.h"
#include "Commands/SaveFile/FileData.h"
#include "Commands/SaveFile/SaveFileCmd.h"
#include "Commands/ServiceCommands/PingCmd.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic_uint64_t sequence{0};
            const auto uniqueValue =
                std::chrono::steady_clock::now().time_since_epoch().count() +
                static_cast<std::int64_t>(
                    sequence.fetch_add(1, std::memory_order_relaxed));
            path_ = fs::temp_directory_path() /
                ("searchengine-command-result-" + std::to_string(uniqueValue));
            fs::create_directories(path_);
        }

        ~TemporaryDirectory()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }

        [[nodiscard]] const fs::path& path() const noexcept
        {
            return path_;
        }

    private:
        fs::path path_;
    };

    class TestSaveFileCmd final : public SaveFileCmd
    {
    public:
        explicit TestSaveFileCmd(fs::path basePath)
            : basePath_(std::move(basePath))
        {
        }

        fs::path getBasePath() override
        {
            return basePath_;
        }

    private:
        fs::path basePath_;
    };

    void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(file.good());
    }

    std::vector<std::uint8_t> readBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        EXPECT_TRUE(file.is_open());
        return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{});
    }
}

TEST(PingCommandResult, ReturnsPongForExactRequest)
{
    PingCmd command;
    const std::vector<std::uint8_t> request{'P', 'I', 'N', 'G'};

    const auto result = command.executeResult(request);

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.payload, (std::vector<std::uint8_t>{'P', 'O', 'N', 'G'}));
}

TEST(PingCommandResult, RejectsUnexpectedPayload)
{
    PingCmd command;

    const auto result = command.executeResult({'P', 'I', 'N'});

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::InvalidRequest);
    EXPECT_TRUE(result.payload.empty());
}

TEST(GetFileCommandResult, RejectsEmptyPath)
{
    const auto result = GetFileCmd::downloadFileResultByPath(std::string{});

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::InvalidRequest);
}

TEST(GetFileCommandResult, DistinguishesMissingFileFromEmptyFile)
{
    TemporaryDirectory temporaryDirectory;
    const fs::path missingPath = temporaryDirectory.path() / "missing.bin";
    const fs::path emptyPath = temporaryDirectory.path() / "empty.bin";
    std::ofstream(emptyPath, std::ios::binary).close();

    const auto missing =
        GetFileCmd::downloadFileResultByPath(missingPath.string());
    const auto empty = GetFileCmd::downloadFileResultByPath(emptyPath.string());

    ASSERT_TRUE(missing.failed());
    EXPECT_EQ(missing.error, command_execution::ErrorCode::FileNotFound);
    ASSERT_TRUE(empty.succeeded());
    EXPECT_TRUE(empty.payload.empty());
}

TEST(GetFileCommandResult, PreservesBinaryContent)
{
    TemporaryDirectory temporaryDirectory;
    const fs::path filePath = temporaryDirectory.path() / "payload.bin";
    const std::vector<std::uint8_t> expected{0x00, 0x01, 0x7f, 0x80, 0xff};
    writeBytes(filePath, expected);

    const auto result = GetFileCmd::downloadFileResultByPath(filePath.string());

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.payload, expected);
}

TEST(GetBinFileDisabled, DoesNotReturnExistingFileContent)
{
    TemporaryDirectory temporaryDirectory;
    const fs::path filePath = temporaryDirectory.path() / "sentinel.bin";
    const std::vector<std::uint8_t> sentinel{
        'S', 'E', 'N', 'T', 'I', 'N', 'E', 'L', '-', 'G', 'E', 'T', 'B', 'I', 'N', 'F', 'I', 'L', 'E'};
    writeBytes(filePath, sentinel);

    const std::string pathStr = filePath.string();
    const std::vector<std::uint8_t> request{pathStr.begin(), pathStr.end()};
    const auto rejected = GetFileCmd::rejectRawBinFileDownload(request);

    ASSERT_TRUE(rejected.failed());
    EXPECT_EQ(rejected.error, command_execution::ErrorCode::InvalidCommand);
    EXPECT_TRUE(rejected.payload.empty());
    EXPECT_NE(rejected.payload, sentinel);
    EXPECT_EQ(readBytes(filePath), sentinel);

    const auto stillReadable = GetFileCmd::downloadFileResultByPath(pathStr);
    ASSERT_TRUE(stillReadable.succeeded());
    EXPECT_EQ(stillReadable.payload, sentinel);
}

TEST(SaveFileCommandResult, RejectsMalformedArchive)
{
    TemporaryDirectory temporaryDirectory;
    TestSaveFileCmd command(temporaryDirectory.path());

    const auto result = command.executeResult({0x01, 0x02, 0x03});

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::InvalidBinaryPayload);
}

TEST(SaveFileCommandResult, RejectsEmptyFilename)
{
    TemporaryDirectory temporaryDirectory;
    TestSaveFileCmd command(temporaryDirectory.path());
    const auto request = serializeToBytes(FileData("", {0x01}));

    const auto result = command.executeResult(request);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::InvalidRequest);
}

TEST(SaveFileCommandResult, SavesPayloadAndKeepsLegacySuccessByte)
{
    TemporaryDirectory temporaryDirectory;
    TestSaveFileCmd command(temporaryDirectory.path());
    const std::vector<std::uint8_t> expected{0x00, 0x10, 0xff};
    const auto request = serializeToBytes(FileData("payload.bin", expected));

    const auto result = command.executeResult(request);

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.payload, (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(readBytes(temporaryDirectory.path() / "payload.bin"), expected);
}

TEST(SaveFileCommandResult, SavesEmptyFileAsSuccessfulPayload)
{
    TemporaryDirectory temporaryDirectory;
    TestSaveFileCmd command(temporaryDirectory.path());
    const auto request = serializeToBytes(FileData("empty.bin", {}));

    const auto result = command.executeResult(request);

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.payload, (std::vector<std::uint8_t>{1}));
    EXPECT_TRUE(fs::exists(temporaryDirectory.path() / "empty.bin"));
    EXPECT_TRUE(readBytes(temporaryDirectory.path() / "empty.bin").empty());
}

TEST(SaveFileCommandResult, RejectsNonDirectoryBasePath)
{
    TemporaryDirectory temporaryDirectory;
    const fs::path basePath = temporaryDirectory.path() / "not-a-directory";
    writeBytes(basePath, {0x01});
    TestSaveFileCmd command(basePath);
    const auto request = serializeToBytes(FileData("payload.bin", {0x02}));

    const auto result = command.executeResult(request);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, command_execution::ErrorCode::ConfigurationError);
}
