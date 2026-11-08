#include <gtest/gtest.h>

#include "TokenLoader.hpp"

#include <fstream>
#include <filesystem>
#include <string>

namespace {

std::string writeTempToken(const std::string& json_body)
{
    const auto* test_info = testing::UnitTest::GetInstance()->current_test_info();
    const std::string path =
        (std::filesystem::temp_directory_path() /
         (std::string("token-loader-") + test_info->name() + ".json"))
            .string();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json_body;
    output.close();
    return path;
}

std::string validTokenJson(const std::string& signature_block)
{
    return R"({
  "format": "searchclient-auth-token",
  "format_version": 1,
  "client_id": "C-001",
  "client_name": "Test User",
  "device_type": "usb",
  "device_id": "usb-serial-1",
  "signature": )" +
        signature_block + R"(
})";
}

} // namespace

TEST(TokenLoader, AcceptsBase64Encoding)
{
    const auto path = writeTempToken(validTokenJson(
        R"({
    "alg": "RS256",
    "encoding": "base64",
    "value": "dGVzdA=="
  })"));

    const auto fields = auth_db::loadTokenFields(path);
    EXPECT_EQ(fields.client_id, "C-001");
    EXPECT_EQ(fields.device_type, "usb");
    EXPECT_EQ(fields.device_id, "USB-SERIAL-1");
}

TEST(TokenLoader, RejectsMissingEncoding)
{
    const auto path = writeTempToken(validTokenJson(
        R"({
    "alg": "RS256",
    "value": "dGVzdA=="
  })"));

    EXPECT_THROW(auth_db::loadTokenFields(path), std::runtime_error);
}

TEST(TokenLoader, RejectsEmptyEncoding)
{
    const auto path = writeTempToken(validTokenJson(
        R"({
    "alg": "RS256",
    "encoding": "",
    "value": "dGVzdA=="
  })"));

    EXPECT_THROW(auth_db::loadTokenFields(path), std::runtime_error);
}

TEST(TokenLoader, RejectsNonBase64Encoding)
{
    const auto path = writeTempToken(validTokenJson(
        R"({
    "alg": "RS256",
    "encoding": "hex",
    "value": "dGVzdA=="
  })"));

    EXPECT_THROW(auth_db::loadTokenFields(path), std::runtime_error);
}
