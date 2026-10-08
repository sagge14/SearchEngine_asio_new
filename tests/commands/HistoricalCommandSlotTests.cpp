#include <gtest/gtest.h>

#include "AsioServer/AsioServer.h"
#include "Commands/CommandResult.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    using asio_server::COMMAND;
    using command_execution::ErrorCode;

    [[nodiscard]] std::string readTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
}

TEST(HistoricalCommandSlots, WireOrdinalsRemainStable)
{
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::JSONREGUEST), 3u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::ADDRESOLUTION), 4u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::UPDATE), 5u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETRESOLUTIONS), 6u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETRESOLUTION), 7u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETDOCS), 8u);
    EXPECT_EQ(static_cast<std::uint_fast64_t>(COMMAND::GETDOC), 9u);
}

TEST(HistoricalCommandSlots, AreNotActiveRequestCommands)
{
    for (const auto command : {
             COMMAND::JSONREGUEST,
             COMMAND::ADDRESOLUTION,
             COMMAND::UPDATE,
             COMMAND::GETRESOLUTIONS,
             COMMAND::GETRESOLUTION,
             COMMAND::GETDOCS,
             COMMAND::GETDOC})
    {
        EXPECT_FALSE(asio_server::isRequestCommand(command))
            << asio_server::getTextCommand(command);
        EXPECT_FALSE(asio_server::isSessionBootstrapCommand(command))
            << asio_server::getTextCommand(command);
    }
}

TEST(HistoricalCommandSlots, ReadLoopRejectsBeforeBodyAllocationAndClosesSession)
{
    const std::filesystem::path repoRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const std::filesystem::path server = repoRoot / "src/AsioServer/AsioServer.cpp";
    ASSERT_TRUE(std::filesystem::exists(server));

    const auto serverText = readTextFile(server);
    const auto trustRejectPos = serverText.find("if (!trustCommand(requestHeader))");
    const auto genericAllocPos =
        serverText.find("std::vector<BYTE> requestData(requestHeader.size");
    ASSERT_NE(trustRejectPos, std::string::npos);
    ASSERT_NE(genericAllocPos, std::string::npos);
    EXPECT_LT(trustRejectPos, genericAllocPos);

    EXPECT_NE(
        serverText.find("command_execution::ErrorCode::InvalidCommand"),
        std::string::npos);
    EXPECT_NE(serverText.find("wire_command="), std::string::npos);

    const auto trustBlock = serverText.substr(
        trustRejectPos,
        genericAllocPos - trustRejectPos);
    EXPECT_NE(trustBlock.find("true"), std::string::npos)
        << "historical slots must close the session after InvalidCommand";
}

TEST(HistoricalCommandSlots, TypedAndLegacyInvalidCommandContracts)
{
    const auto typed = asio_server::makeTypedErrorResponse(ErrorCode::InvalidCommand);
    EXPECT_EQ(
        typed.errorCode,
        static_cast<std::uint32_t>(ErrorCode::InvalidCommand));
    EXPECT_EQ(
        asio_server::legacyErrorCommand(ErrorCode::InvalidCommand),
        COMMAND::SOMEERROR);
}
