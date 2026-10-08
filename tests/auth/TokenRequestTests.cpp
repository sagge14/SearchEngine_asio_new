#include <gtest/gtest.h>

#include "TokenDocument.hpp"
#include "TokenLoader.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

token_issuer::TokenFields requestFields()
{
    token_issuer::TokenFields fields;
    fields.client_id = " PC-005 ";
    fields.client_name = " Test User ";
    fields.device_type = "computer";
    fields.device_id = "{a1b2c3d4-e5f6-7890-abcd-ef1234567890}";
    return fields;
}

class RequestFile {
public:
    RequestFile()
        : path(std::filesystem::temp_directory_path() /
               (std::string("computer-request-") +
                testing::UnitTest::GetInstance()->current_test_info()->name() +
                ".json"))
    {}
    ~RequestFile()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    std::filesystem::path path;
};

} // namespace

TEST(TokenRequest, RoundTripPreservesRequestedComputerIdentity)
{
    const auto document = token_issuer::BuildComputerRequestDocument(requestFields());
    EXPECT_EQ(document.size(), 6u);
    EXPECT_EQ(document.at("format"), "searchclient-auth-request");
    EXPECT_FALSE(document.contains("signature"));
    EXPECT_FALSE(document.contains("issuer"));
    const auto fields = token_issuer::ParseComputerRequestDocument(document);
    EXPECT_EQ(fields.client_id, "PC-005");
    EXPECT_EQ(fields.client_name, "Test User");
    EXPECT_EQ(fields.device_id, "A1B2C3D4-E5F6-7890-ABCD-EF1234567890");
    EXPECT_EQ(fields.device_type, "computer");
}

TEST(TokenRequest, RejectsUsbAndUnusableComputerIdentifiers)
{
    auto fields = requestFields();
    fields.device_type = "usb";
    EXPECT_THROW(token_issuer::BuildComputerRequestDocument(fields), std::runtime_error);
    fields.device_type = "computer";
    for (const auto* uuid : {"", "invalid", "00000000-0000-0000-0000-000000000000",
                             "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"}) {
        fields.device_id = uuid;
        EXPECT_THROW(token_issuer::BuildComputerRequestDocument(fields), std::runtime_error);
    }
}

TEST(TokenRequest, RejectsSignedDocumentsAndAdditionalFields)
{
    const auto original = token_issuer::BuildComputerRequestDocument(requestFields());
    auto document = original;
    document["format"] = token_issuer::kTokenFormat;
    EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
    for (const auto* key : {"signature", "private_key", "password", "issuer", "expires_at"}) {
        document = original;
        document[key] = "unexpected";
        EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
    }
}

TEST(TokenRequest, RejectsMissingFieldsWrongTypesAndUnsupportedVersions)
{
    const auto original = token_issuer::BuildComputerRequestDocument(requestFields());
    for (const auto* key : {"format", "format_version", "client_id", "client_name",
                             "device_type", "device_id"}) {
        auto document = original;
        document.erase(key);
        EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
        document = original;
        document[key] = nullptr;
        EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
    }
    for (const nlohmann::json version : {nlohmann::json(2), nlohmann::json(1.0),
                                       nlohmann::json("1")}) {
        auto document = original;
        document["format_version"] = version;
        EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
    }
    auto document = original;
    document["client_name"] = "bad\nname";
    EXPECT_THROW(token_issuer::ParseComputerRequestDocument(document), std::runtime_error);
}

TEST(TokenRequest, UnsignedFileCannotBeImportedAsAuthToken)
{
    RequestFile file;
    token_issuer::WriteComputerRequestFile(file.path, requestFields());
    const auto fields = token_issuer::LoadComputerRequestFile(file.path);
    EXPECT_EQ(fields.client_id, "PC-005");
    EXPECT_THROW(auth_db::loadTokenFields(file.path.string()), std::runtime_error);

    // Even renaming the document format does not make an unsigned file usable.
    auto document = token_issuer::BuildComputerRequestDocument(fields);
    document["format"] = token_issuer::kTokenFormat;
    { std::ofstream output(file.path); output << document; }
    EXPECT_THROW(auth_db::loadTokenFields(file.path.string()), std::runtime_error);

    token_issuer::TokenSignature signature;
    signature.value = "dGVzdA==";
    token_issuer::WriteTokenFile(file.path, fields, signature);
    const auto imported = auth_db::loadTokenFields(file.path.string());
    EXPECT_EQ(imported.client_id, fields.client_id);
    EXPECT_EQ(imported.device_id, fields.device_id);
    EXPECT_EQ(imported.device_type, "computer");
    EXPECT_THROW(token_issuer::LoadComputerRequestFile(file.path), std::runtime_error);
}

TEST(TokenRequest, FileReaderRejectsOversizeEmptyAndInvalidJson)
{
    RequestFile file;
    for (const auto& data : {std::string(), std::string("{broken"),
                            std::string(65537, ' ')}) {
        { std::ofstream output(file.path, std::ios::binary); output << data; }
        EXPECT_THROW(token_issuer::LoadComputerRequestFile(file.path), std::runtime_error);
    }
}
