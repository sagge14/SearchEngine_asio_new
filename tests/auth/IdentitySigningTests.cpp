#include <gtest/gtest.h>

#include "Auth/IdentitySigning.h"
#include "IdentitySigning.hpp"

TEST(IdentitySigning, ServerAndIssuerProduceIdenticalBytes)
{
    const std::string client_id = "C-001";
    const std::string client_name = "Ivanov I.I.";
    const std::string device_type = "usb";
    const std::string device_id = "USB-SERIAL-1";

    const auto server = auth::BuildIdentitySigningMessage(
        client_id, client_name, device_type, device_id);
    const auto issuer = token_issuer::BuildIdentitySigningMessage(
        client_id, client_name, device_type, device_id);

    EXPECT_EQ(server, issuer);
    EXPECT_EQ(server, "C-001\nIvanov I.I.\nusb\nUSB-SERIAL-1\n");
    EXPECT_EQ(server.back(), '\n');
}

TEST(IdentitySigning, ComputerPayloadKeepsFinalNewline)
{
    const auto message = auth::BuildIdentitySigningMessage(
        "pc-1",
        "desk-pc",
        "computer",
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
    EXPECT_EQ(
        message,
        "pc-1\ndesk-pc\ncomputer\nA1B2C3D4-E5F6-7890-ABCD-EF1234567890\n");
    EXPECT_EQ(
        message,
        token_issuer::BuildIdentitySigningMessage(
            "pc-1",
            "desk-pc",
            "computer",
            "A1B2C3D4-E5F6-7890-ABCD-EF1234567890"));
}
