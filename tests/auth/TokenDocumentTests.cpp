#include <gtest/gtest.h>

#include "TokenDocument.hpp"

#include <stdexcept>

TEST(TokenDocument, UsbTokenContainsDeviceTypeAndId)
{
    token_issuer::TokenFields fields;
    fields.client_id = "C-001";
    fields.client_name = "Ivanov I.I.";
    fields.device_type = "usb";
    fields.device_id = "usb-serial-1";
    fields.issued_at = "2026-01-01T00:00:00Z";
    fields.issuer = "auth-server";

    token_issuer::TokenSignature signature;
    signature.value = "dGVzdA==";

    const auto document = token_issuer::BuildTokenDocument(fields, signature);
    EXPECT_EQ(document.at("format_version").get<int>(), 1);
    EXPECT_EQ(document.at("device_type").get<std::string>(), "usb");
    EXPECT_EQ(document.at("device_id").get<std::string>(), "USB-SERIAL-1");
    EXPECT_FALSE(document.contains("flash_serial"));
}

TEST(TokenDocument, ComputerTokenContainsNormalizedUuid)
{
    token_issuer::TokenFields fields;
    fields.client_id = "pc-1";
    fields.client_name = "desk-pc";
    fields.device_type = "computer";
    fields.device_id = "{a1b2c3d4-e5f6-7890-abcd-ef1234567890}";
    fields.issued_at = "2026-01-01T00:00:00Z";
    fields.issuer = "auth-server";

    token_issuer::TokenSignature signature;
    signature.value = "dGVzdA==";

    const auto document = token_issuer::BuildTokenDocument(fields, signature);
    EXPECT_EQ(document.at("format_version").get<int>(), 1);
    EXPECT_EQ(document.at("device_type").get<std::string>(), "computer");
    EXPECT_EQ(
        document.at("device_id").get<std::string>(),
        "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
}

TEST(TokenDocument, RejectsZeroComputerUuid)
{
    token_issuer::TokenFields fields;
    fields.client_id = "pc-1";
    fields.client_name = "desk-pc";
    fields.device_type = "computer";
    fields.device_id = "00000000-0000-0000-0000-000000000000";

    token_issuer::TokenSignature signature;
    signature.value = "dGVzdA==";
    EXPECT_THROW(
        token_issuer::BuildTokenDocument(fields, signature),
        std::runtime_error);
}
