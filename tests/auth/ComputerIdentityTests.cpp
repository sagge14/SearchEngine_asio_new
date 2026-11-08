#include <gtest/gtest.h>

#include "Auth/DeviceIdentity.h"

TEST(ComputerIdentity, NormalizesCanonicalUuid)
{
    const auto uuid = auth::NormalizeComputerUuid(
        "  {a1b2c3d4-e5f6-7890-abcd-ef1234567890}  ");
    ASSERT_TRUE(uuid.has_value());
    EXPECT_EQ(*uuid, "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
}

TEST(ComputerIdentity, Accepts32HexDigitsWithoutHyphens)
{
    const auto uuid = auth::NormalizeComputerUuid(
        "a1b2c3d4e5f67890abcdef1234567890");
    ASSERT_TRUE(uuid.has_value());
    EXPECT_EQ(*uuid, "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
}

TEST(ComputerIdentity, RejectsEmpty)
{
    EXPECT_FALSE(auth::NormalizeComputerUuid("").has_value());
    EXPECT_FALSE(auth::NormalizeComputerUuid("   ").has_value());
}

TEST(ComputerIdentity, RejectsZeroUuid)
{
    EXPECT_FALSE(
        auth::NormalizeComputerUuid("00000000-0000-0000-0000-000000000000")
            .has_value());
    EXPECT_FALSE(auth::IsUsableComputerUuid("{00000000-0000-0000-0000-000000000000}"));
}

TEST(ComputerIdentity, RejectsAllFUuid)
{
    EXPECT_FALSE(
        auth::NormalizeComputerUuid("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF")
            .has_value());
    EXPECT_FALSE(auth::IsUsableComputerUuid("ffffffffffffffffffffffffffffffff"));
}

TEST(ComputerIdentity, RejectsNonHex)
{
    EXPECT_FALSE(
        auth::NormalizeComputerUuid("GGGGGGGG-0000-0000-0000-000000000000")
            .has_value());
}

TEST(DeviceIdentity, SupportsUsbAndComputerOnly)
{
    EXPECT_TRUE(auth::IsSupportedDeviceType("usb"));
    EXPECT_TRUE(auth::IsSupportedDeviceType("computer"));
    EXPECT_FALSE(auth::IsSupportedDeviceType("flash"));
    EXPECT_FALSE(auth::IsSupportedDeviceType(""));
}

TEST(DeviceIdentity, UsbDeviceIdIsTrimmedUppercase)
{
    EXPECT_EQ(auth::NormalizeUsbDeviceId("  abc-123  "), "ABC-123");
}
