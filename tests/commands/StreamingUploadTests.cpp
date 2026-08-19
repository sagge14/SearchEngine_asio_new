#include "AsioServer/AsioServer.h"
#include "Commands/StreamingUpload/StreamingUpload.h"
#include "MyUtils/Utf8Path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace
{
    namespace fs = std::filesystem;
    namespace nh = nlohmann;
    using command_execution::ErrorCode;
    using streaming_upload::Metadata;
    using streaming_upload::PlannedTarget;
    using streaming_upload::StreamingUploadSink;
    using streaming_upload::TimeParts;

    std::vector<std::uint8_t> jsonBytes(const nh::json& document)
    {
        const std::string serialized = document.dump();
        return {serialized.begin(), serialized.end()};
    }

    nh::json validMetadataJson(
        std::string_view fileName = "note.txt",
        std::uint64_t fileSize = 4)
    {
        return nh::json{
            {"version", 1},
            {"file_name", fileName},
            {"file_size", fileSize}};
    }

    std::string readTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

    void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        if (!bytes.empty()) {
            file.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        ASSERT_TRUE(file.good());
    }

    std::vector<std::uint8_t> readAllBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        EXPECT_TRUE(file.is_open());
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

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
                ("searchengine-streaming-upload-" + std::to_string(uniqueValue));
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

    command_execution::CommandResult parseJson(const nh::json& document, Metadata& out)
    {
        const auto bytes = jsonBytes(document);
        return streaming_upload::parseMetadata(bytes, out);
    }

    command_execution::CommandResult uploadExact(
        StreamingUploadSink& sink,
        const PlannedTarget& target,
        const std::vector<std::uint8_t>& bytes)
    {
        auto prepared = sink.prepare(target, bytes.size());
        if (prepared.failed())
            return prepared;
        constexpr std::size_t chunkSize = streaming_upload::kChunkSize;
        for (std::size_t offset = 0; offset < bytes.size(); offset += chunkSize) {
            const std::size_t n = (std::min)(chunkSize, bytes.size() - offset);
            auto written = sink.writeChunk(
                std::span<const std::uint8_t>(bytes.data() + offset, n));
            if (written.failed())
                return written;
        }
        return sink.publish();
    }
}

TEST(StreamingUploadOrdinals, NewCommandsKeepStableWireValues)
{
    using asio_server::COMMAND;
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::LOAD_TLG_TO_SEND), 15u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::LOAD_RAZN), 22u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::UPLOAD_TLG_TO_SEND_V1), 34u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::UPLOAD_RAZN_V1), 35u);
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::UPLOAD_TLG_TO_SEND_V1));
    EXPECT_TRUE(asio_server::isRequestCommand(COMMAND::UPLOAD_RAZN_V1));
    EXPECT_TRUE(asio_server::isStreamingUploadCommand(COMMAND::UPLOAD_TLG_TO_SEND_V1));
    EXPECT_TRUE(asio_server::isStreamingUploadCommand(COMMAND::UPLOAD_RAZN_V1));
    EXPECT_FALSE(asio_server::isStreamingUploadCommand(COMMAND::LOAD_TLG_TO_SEND));
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::UPLOAD_TLG_TO_SEND_V1));
    EXPECT_FALSE(asio_server::isSessionBootstrapCommand(COMMAND::UPLOAD_RAZN_V1));
    EXPECT_FALSE(
        asio_server::evaluateSessionCommandGate(
            COMMAND::UPLOAD_TLG_TO_SEND_V1, false).allow_execute);
    EXPECT_TRUE(
        asio_server::evaluateSessionCommandGate(
            COMMAND::UPLOAD_RAZN_V1, true).allow_execute);
}

TEST(StreamingUploadContract, SourceHandlesCommandsBeforeGenericPayloadAlloc)
{
    const fs::path repoRoot =
        fs::path(__FILE__).parent_path().parent_path().parent_path();
    const fs::path server = repoRoot / "src/AsioServer/AsioServer.cpp";
    const fs::path helper =
        repoRoot / "src/Commands/StreamingUpload/StreamingUpload.cpp";
    ASSERT_TRUE(fs::exists(server));
    ASSERT_TRUE(fs::exists(helper));

    const auto serverText = readTextFile(server);
    const auto helperText = readTextFile(helper);

    const auto callPos = serverText.find("handleStreamingUpload(requestHeader)");
    const auto genericPos =
        serverText.find("std::vector<BYTE> requestData(requestHeader.size");
    ASSERT_NE(callPos, std::string::npos);
    ASSERT_NE(genericPos, std::string::npos);
    EXPECT_LT(callPos, genericPos);

    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::LOAD_TLG_TO_SEND]"),
        std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::LOAD_RAZN]"),
        std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::UPLOAD_TLG_TO_SEND_V1]"),
        std::string::npos);
    EXPECT_EQ(
        serverText.find("cmdMap[COMMAND::UPLOAD_RAZN_V1]"),
        std::string::npos);
    EXPECT_NE(
        serverText.find("legacy upload is disabled"),
        std::string::npos);

    EXPECT_EQ(helperText.find("FileData"), std::string::npos);
    EXPECT_EQ(helperText.find("deserializeFromBytes"), std::string::npos);
    EXPECT_EQ(serverText.find("deserializeFromBytes"), std::string::npos);
}

TEST(StreamingUploadMetadata, RejectsMalformedJson)
{
    Metadata metadata;
    const std::vector<std::uint8_t> bytes{'{', 'b', 'a', 'd'};
    const auto result = streaming_upload::parseMetadata(bytes, metadata);
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error, ErrorCode::InvalidJson);
}

TEST(StreamingUploadMetadata, RejectsMissingAndWrongFields)
{
    Metadata metadata;
    EXPECT_EQ(
        parseJson({{"file_name", "a.txt"}, {"file_size", 1}}, metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson({{"version", 1}, {"file_size", 1}}, metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson({{"version", 1}, {"file_name", "a.txt"}}, metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson(
            {{"version", 2}, {"file_name", "a.txt"}, {"file_size", 1}},
            metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson(
            {{"version", 1}, {"file_name", 3}, {"file_size", 1}},
            metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson(
            {{"version", 1}, {"file_name", "a.txt"}, {"file_size", -1}},
            metadata).error,
        ErrorCode::InvalidRequest);
    EXPECT_EQ(
        parseJson(
            {{"version", 1}, {"file_name", "a.txt"}, {"file_size", 1.5}},
            metadata).error,
        ErrorCode::InvalidRequest);
}

TEST(StreamingUploadMetadata, RejectsRoutingFields)
{
    const char* forbidden[] = {
        "path",
        "dir",
        "directory",
        "user",
        "operator",
        "target",
        "destination",
        "remotePath"};
    for (const char* key : forbidden) {
        auto document = validMetadataJson();
        document[key] = "x";
        Metadata metadata;
        const auto result = parseJson(document, metadata);
        EXPECT_TRUE(result.failed()) << key;
        EXPECT_EQ(result.error, ErrorCode::InvalidRequest) << key;
    }
}

TEST(StreamingUploadMetadata, AcceptsExactContractIncludingZeroSize)
{
    Metadata metadata;
    auto result = parseJson(validMetadataJson("note.txt", 0), metadata);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(metadata.version, 1u);
    EXPECT_EQ(metadata.file_name, "note.txt");
    EXPECT_EQ(metadata.file_size, 0u);

    result = parseJson(validMetadataJson("note.txt", 123456), metadata);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(metadata.file_size, 123456u);
}

TEST(StreamingUploadBasename, RejectsTraversalAndUnsafeForms)
{
    const char* unsafe[] = {
        "../x",
        "..\\x",
        "C:\\x",
        "C:x",
        "\\x",
        "/x",
        "dir/x",
        "dir\\x",
        "abc:stream",
        ".",
        "..",
        "NUL",
        "CON",
        "PRN",
        "AUX",
        "COM1",
        "LPT1",
        "con.txt",
        "file.txt ",
        "file.txt.",
        "a<b.txt",
        ""};
    for (const char* name : unsafe) {
        EXPECT_FALSE(streaming_upload::isSafeBasename(name)) << name;
        EXPECT_FALSE(streaming_upload::isSafeOperatorComponent(name)) << name;
    }

    std::string embeddedNul("a");
    embeddedNul.push_back('\0');
    embeddedNul += "b.txt";
    EXPECT_FALSE(streaming_upload::isSafeBasename(embeddedNul));

    auto document = validMetadataJson();
    document["file_name"] = std::string("x") + '\0' + "y.txt";
    Metadata metadata;
    EXPECT_TRUE(parseJson(document, metadata).failed());
}

TEST(StreamingUploadBasename, AcceptsUnicodeName)
{
    EXPECT_TRUE(streaming_upload::isSafeBasename("файл.txt"));
    Metadata metadata;
    const auto result = parseJson(validMetadataJson("файл.txt", 0), metadata);
    ASSERT_TRUE(result.succeeded()) << result.diagnostic;
    EXPECT_EQ(metadata.file_name, "файл.txt");
}

TEST(StreamingUploadRouting, RaznStaysUnderConfiguredRoot)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "report.txt";
    metadata.file_size = 0;
    PlannedTarget target;
    const auto planned = streaming_upload::planRaznTarget(root, metadata, target);
    ASSERT_TRUE(planned.succeeded()) << planned.diagnostic;
    EXPECT_EQ(target.baseName, "report.txt");
    EXPECT_EQ(target.directory.lexically_normal(), root.lexically_normal());
    EXPECT_TRUE(streaming_upload::compositionStaysUnderDirectory(
        root, target.directory / encoding::utf8_to_wstring(target.baseName)));

    PlannedTarget ignored;
    EXPECT_EQ(
        streaming_upload::planRaznTarget(fs::path("relative"), metadata, ignored)
            .error,
        ErrorCode::ConfigurationError);
}

TEST(StreamingUploadRouting, TlgUsesServerOperatorAndBusinessDateFolders)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "tlg";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "tlg.bin";
    metadata.file_size = 1;
    TimeParts time;
    time.monthUpper = encoding::utf8_to_wstring("ЯНВАРЬ");
    time.date = L"19.08.26";
    time.hhmm = L"05-03";
    PlannedTarget target;
    const auto planned = streaming_upload::planTlgTarget(
        root, "admin", metadata, time, target);
    ASSERT_TRUE(planned.succeeded()) << planned.diagnostic;
    const fs::path expected = root / time.monthUpper / time.date /
        encoding::utf8_to_wstring("admin_05-03");
    EXPECT_EQ(target.directory.lexically_normal(), expected.lexically_normal());
    EXPECT_TRUE(fs::is_directory(target.directory));
    EXPECT_TRUE(streaming_upload::compositionStaysUnderDirectory(
        root, target.directory / encoding::utf8_to_wstring(target.baseName)));
}

TEST(StreamingUploadRouting, RejectsUnsafeOperator)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "tlg";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "tlg.bin";
    TimeParts time;
    time.monthUpper = encoding::utf8_to_wstring("ЯНВАРЬ");
    time.date = L"19.08.26";
    time.hhmm = L"05-03";
    PlannedTarget target;
    const char* unsafeOps[] = {"", ".", "..", "a/b", "a\\b", "CON", "admin:1"};
    for (const char* op : unsafeOps) {
        const auto planned =
            streaming_upload::planTlgTarget(root, op, metadata, time, target);
        EXPECT_TRUE(planned.failed()) << op;
        EXPECT_EQ(planned.error, ErrorCode::OperatorNotRegistered) << op;
    }
}

TEST(StreamingUploadSink, ZeroSizeAndUnicodeBasename)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "файл.txt";
    metadata.file_size = 0;
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    auto published = uploadExact(sink, target, {});
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "файл.txt");
    const fs::path publishedPath = sink.publishedPath();
    ASSERT_TRUE(fs::exists(publishedPath));
    EXPECT_EQ(fs::file_size(publishedPath), 0u);
    EXPECT_TRUE(streaming_upload::compositionStaysUnderDirectory(root, publishedPath));
}

TEST(StreamingUploadSink, SameSizeExistingFileIsNotOverwritten)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    const std::vector<std::uint8_t> original{'A', 'A', 'A', 'A'};
    const std::vector<std::uint8_t> incoming{'B', 'B', 'B', 'B'};
    writeBytes(root / "file.ext", original);

    Metadata metadata;
    metadata.file_name = "file.ext";
    metadata.file_size = incoming.size();
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    auto published = uploadExact(sink, target, incoming);
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "file(1).ext");
    EXPECT_EQ(readAllBytes(root / "file.ext"), original);
    EXPECT_EQ(readAllBytes(root / "file(1).ext"), incoming);
}

TEST(StreamingUploadSink, DifferentSizeExistingFileIsPreserved)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    const std::vector<std::uint8_t> original{'A', 'A'};
    const std::vector<std::uint8_t> incoming{'B', 'B', 'B', 'B'};
    writeBytes(root / "file.ext", original);

    Metadata metadata;
    metadata.file_name = "file.ext";
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    auto published = uploadExact(sink, target, incoming);
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "file(1).ext");
    EXPECT_EQ(readAllBytes(root / "file.ext"), original);
    EXPECT_EQ(readAllBytes(root / "file(1).ext"), incoming);
}

TEST(StreamingUploadSink, MultipleCollisionsUseIncreasingSuffix)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    writeBytes(root / "file.ext", {'1'});
    writeBytes(root / "file(1).ext", {'2'});
    writeBytes(root / "file(2).ext", {'3'});
    const std::vector<std::uint8_t> incoming{'4'};

    Metadata metadata;
    metadata.file_name = "file.ext";
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    auto published = uploadExact(sink, target, incoming);
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "file(3).ext");
    EXPECT_EQ(readAllBytes(root / "file.ext"), (std::vector<std::uint8_t>{'1'}));
    EXPECT_EQ(readAllBytes(root / "file(1).ext"), (std::vector<std::uint8_t>{'2'}));
    EXPECT_EQ(readAllBytes(root / "file(2).ext"), (std::vector<std::uint8_t>{'3'}));
    EXPECT_EQ(readAllBytes(root / "file(3).ext"), incoming);
}

TEST(StreamingUploadSink, PartialAbortRemovesStagingAndLeavesFinalAbsent)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "partial.bin";
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    const std::array<std::uint8_t, 32> chunk{};
    auto prepared = sink.prepare(target, 1000);
    ASSERT_TRUE(prepared.succeeded()) << prepared.diagnostic;
    ASSERT_TRUE(sink.writeChunk(chunk).succeeded());
    sink.abort();

    EXPECT_FALSE(fs::exists(root / "partial.bin"));
    bool leftoverStaging = false;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L".se-upload-", 0) == 0)
            leftoverStaging = true;
    }
    EXPECT_FALSE(leftoverStaging);
}

TEST(StreamingUploadSink, SixtyFourMiBChunkedStreamDoesNotNeedFullVector)
{
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    fs::create_directories(root);
    Metadata metadata;
    metadata.file_name = "big.bin";
    constexpr std::uint64_t kSize = 64ull * 1024ull * 1024ull;
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    auto prepared = sink.prepare(target, kSize);
    ASSERT_TRUE(prepared.succeeded()) << prepared.diagnostic;

    std::array<std::uint8_t, streaming_upload::kChunkSize> chunk{};
    std::uint64_t offset = 0;
    std::uint64_t checksum = 0;
    while (offset < kSize) {
        const std::size_t n = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(chunk.size()), kSize - offset));
        for (std::size_t i = 0; i < n; ++i) {
            chunk[i] = static_cast<std::uint8_t>((offset + i) & 0xFFu);
            checksum += chunk[i];
        }
        auto written = sink.writeChunk(std::span<const std::uint8_t>(chunk.data(), n));
        ASSERT_TRUE(written.succeeded()) << written.diagnostic;
        offset += n;
    }
    auto published = sink.publish();
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "big.bin");
    EXPECT_EQ(fs::file_size(sink.publishedPath()), kSize);

    std::ifstream in(sink.publishedPath(), std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::uint64_t verified = 0;
    std::uint64_t readOffset = 0;
    while (readOffset < kSize) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        ASSERT_GT(got, 0u);
        bool mismatch = false;
        for (std::size_t i = 0; i < got; ++i) {
            if (chunk[i] != static_cast<std::uint8_t>((readOffset + i) & 0xFFu))
                mismatch = true;
            verified += chunk[i];
        }
        EXPECT_FALSE(mismatch);
        readOffset += got;
    }
    EXPECT_EQ(readOffset, kSize);
    EXPECT_EQ(verified, checksum);
}

TEST(StreamingUploadSink, RejectsReparseEscapeWhenAvailable)
{
#ifdef _WIN32
    TemporaryDirectory temp;
    const fs::path root = temp.path() / "razn";
    const fs::path outside = temp.path() / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    writeBytes(outside / "secret.bin", {'S'});
    const fs::path linkPath = root / "file.ext";
    const DWORD flags = 0x2;
    if (!CreateSymbolicLinkW(
            linkPath.c_str(),
            (outside / "secret.bin").c_str(),
            flags) &&
        !CreateSymbolicLinkW(
            linkPath.c_str(),
            (outside / "secret.bin").c_str(),
            0)) {
        GTEST_SKIP() << "symlink creation is not permitted";
    }

    Metadata metadata;
    metadata.file_name = "file.ext";
    PlannedTarget target;
    ASSERT_TRUE(streaming_upload::planRaznTarget(root, metadata, target).succeeded());
    StreamingUploadSink sink;
    const std::vector<std::uint8_t> incoming{'N', 'E', 'W'};
    auto published = uploadExact(sink, target, incoming);
    ASSERT_TRUE(published.succeeded()) << published.diagnostic;
    EXPECT_EQ(sink.savedName(), "file(1).ext");
    EXPECT_EQ(readAllBytes(outside / "secret.bin"), (std::vector<std::uint8_t>{'S'}));
    EXPECT_EQ(readAllBytes(root / "file(1).ext"), incoming);

    const DWORD attributes = GetFileAttributesW(linkPath.c_str());
    ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_NE(attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
#else
    GTEST_SKIP() << "reparse-point check is Windows-specific";
#endif
}
