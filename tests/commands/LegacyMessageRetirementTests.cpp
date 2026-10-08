#include <gtest/gtest.h>

#include "AsioServer/AsioServer.h"

#include <cstdint>
#include <vector>

namespace
{
using asio_server::COMMAND;
}

TEST(LegacyMessageRetirement, WireSlotsStayReserved)
{
    EXPECT_EQ(static_cast<std::uint64_t>(COMMAND::GET_MESSAGE), 16ULL);
    EXPECT_EQ(static_cast<std::uint64_t>(COMMAND::SAVE_MESSAGE_TO), 2781032419ULL);
}

TEST(LegacyMessageRetirement, GetMessageIsNotActiveRequestCommand)
{
    EXPECT_FALSE(asio_server::isRequestCommand(COMMAND::GET_MESSAGE));
}

TEST(LegacyMessageRetirement, DisabledClassifierRecognizesLegacyMessageCommands)
{
    COMMAND normalized = COMMAND::SOMEERROR;
    EXPECT_TRUE(asio_server::isDisabledLegacyMessageWireCommand(
        COMMAND::GET_MESSAGE, normalized));
    EXPECT_EQ(normalized, COMMAND::GET_MESSAGE);

    normalized = COMMAND::SOMEERROR;
    EXPECT_TRUE(asio_server::isDisabledLegacyMessageWireCommand(
        COMMAND::SAVE_MESSAGE_TO, normalized));
    EXPECT_EQ(normalized, COMMAND::SAVE_MESSAGE_TO);

    for (const auto userId : {0ULL, 1ULL, 0xFFFFFFFFULL}) {
        const std::uint64_t raw = (2781032419ULL << 32) | userId;
        normalized = COMMAND::SOMEERROR;
        EXPECT_TRUE(asio_server::isDisabledLegacyMessageWireCommand(
            static_cast<COMMAND>(raw), normalized));
        EXPECT_EQ(normalized, COMMAND::SAVE_MESSAGE_TO);
    }
}

TEST(LegacyMessageRetirement, DisabledClassifierDoesNotTouchActiveCommands)
{
    COMMAND normalized = COMMAND::SOMEERROR;
    EXPECT_FALSE(asio_server::isDisabledLegacyMessageWireCommand(
        COMMAND::PING, normalized));
    EXPECT_FALSE(asio_server::isDisabledLegacyMessageWireCommand(
        COMMAND::GET_ATTACHMENTS, normalized));
    EXPECT_FALSE(asio_server::isDisabledLegacyMessageWireCommand(
        COMMAND::LOAD_TLG_TO_SEND, normalized));
}

