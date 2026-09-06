#include "TokenDocument.hpp"
#include "Auth/DeviceIdentity.h"
#include "CryptoStub.hpp"
#include "TokenAscii.hpp"

#include <chrono>
#include <array>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace token_issuer {

nlohmann::json BuildComputerRequestDocument(const TokenFields& fields)
{
    ValidateTokenFields(fields);
    if (TrimCopy(fields.device_type) != auth::kDeviceTypeComputer) {
        throw std::runtime_error("unsigned requests require device_type computer");
    }
    return {
        {"format", kRequestFormat},
        {"format_version", kRequestFormatVersion},
        {"client_id", TrimCopy(fields.client_id)},
        {"client_name", TrimCopy(fields.client_name)},
        {"device_type", auth::kDeviceTypeComputer},
        {"device_id", *auth::NormalizeComputerUuid(fields.device_id)},
    };
}

TokenFields ParseComputerRequestDocument(const nlohmann::json& document)
{
    if (!document.is_object()) {
        throw std::runtime_error("unsigned request must be a JSON object");
    }
    const char* string_keys[] = {
        "format", "client_id", "client_name", "device_type", "device_id"
    };
    for (const char* key : string_keys) {
        if (!document.contains(key) || !document.at(key).is_string()) {
            throw std::runtime_error(
                std::string("unsigned request requires string field: ") + key);
        }
    }
    if (document.at("format") != kRequestFormat) {
        throw std::runtime_error("select an unsigned searchclient-auth-request file");
    }
    if (!document.contains("format_version") ||
        !document.at("format_version").is_number_integer() ||
        document.at("format_version") != kRequestFormatVersion)
    {
        throw std::runtime_error("unsupported unsigned request format_version");
    }
    // Only the six identity/format fields belong in a request. In particular,
    // do not accept signatures, issuer settings, or secret-bearing fields.
    if (document.size() != 6) {
        throw std::runtime_error("unsigned request contains unexpected fields");
    }
    TokenFields fields;
    fields.client_id = document.at("client_id").get<std::string>();
    fields.client_name = document.at("client_name").get<std::string>();
    fields.device_type = document.at("device_type").get<std::string>();
    fields.device_id = document.at("device_id").get<std::string>();
    const auto normalized = BuildComputerRequestDocument(fields);
    fields.client_id = normalized.at("client_id").get<std::string>();
    fields.client_name = normalized.at("client_name").get<std::string>();
    fields.device_type = normalized.at("device_type").get<std::string>();
    fields.device_id = normalized.at("device_id").get<std::string>();
    return fields;
}

TokenFields LoadComputerRequestFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open unsigned request file");
    }
    // Bound reads from files received from another computer.
    std::array<char, 65537> buffer{};
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto length = input.gcount();
    if (input.bad() || length <= 0 || length > 65536) {
        throw std::runtime_error("unsigned request must be 1..65536 bytes");
    }
    const auto document = nlohmann::json::parse(
        buffer.data(), buffer.data() + length, nullptr, false);
    if (document.is_discarded()) {
        throw std::runtime_error("invalid unsigned request JSON");
    }
    return ParseComputerRequestDocument(document);
}

void WriteComputerRequestFile(
    const std::filesystem::path& path, const TokenFields& fields)
{
    const auto document = BuildComputerRequestDocument(fields);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write unsigned request file");
    }
    output << document.dump(2);
    output.close();
    if (!output) {
        throw std::runtime_error("failed writing unsigned request file");
    }
}

std::string NowUtcIso8601()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t tt = clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void ValidateTokenFields(const TokenFields& fields)
{
    if (TrimCopy(fields.client_id).empty()) {
        throw std::runtime_error("client_id must be non-empty");
    }
    if (TrimCopy(fields.client_name).empty()) {
        throw std::runtime_error("client_name must be non-empty");
    }

    const auto device_type = TrimCopy(fields.device_type);
    if (!auth::IsSupportedDeviceType(device_type)) {
        throw std::runtime_error("device_type must be usb or computer");
    }

    std::string normalized_device_id;
    if (device_type == auth::kDeviceTypeUsb) {
        normalized_device_id = auth::NormalizeUsbDeviceId(fields.device_id);
        if (normalized_device_id.empty()) {
            throw std::runtime_error("USB device_id must be non-empty");
        }
    } else {
        auto uuid = auth::NormalizeComputerUuid(fields.device_id);
        if (!uuid) {
            throw std::runtime_error(
                "computer device_id must be a usable SMBIOS UUID");
        }
        normalized_device_id = *uuid;
    }

    const std::string* strings[] = {
        &fields.client_id,
        &fields.client_name,
        &fields.device_type,
        &fields.issued_at,
        &fields.issuer,
        &fields.notes,
    };
    for (const std::string* value : strings) {
        if (!IsAsciiTokenField(*value)) {
            throw std::runtime_error(
                "token fields must be printable ASCII only (no Cyrillic)");
        }
    }
    if (!IsAsciiTokenField(normalized_device_id)) {
        throw std::runtime_error(
            "token fields must be printable ASCII only (no Cyrillic)");
    }
    if (fields.expires_at.is_string()) {
        const auto expires = fields.expires_at.get<std::string>();
        if (!IsAsciiTokenField(expires)) {
            throw std::runtime_error(
                "expires_at must be printable ASCII only");
        }
    } else if (!fields.expires_at.is_null()) {
        throw std::runtime_error("expires_at must be null or string");
    }
}

nlohmann::json BuildTokenDocument(
    const TokenFields& fields,
    const TokenSignature& signature)
{
    ValidateTokenFields(fields);
    if (signature.alg != kSignatureAlg || signature.encoding != "base64" ||
        signature.value.empty())
    {
        throw std::runtime_error(
            "token requires RS256 base64 signature value");
    }

    const auto device_type = TrimCopy(fields.device_type);
    std::string device_id;
    if (device_type == auth::kDeviceTypeUsb) {
        device_id = auth::NormalizeUsbDeviceId(fields.device_id);
    } else {
        device_id = *auth::NormalizeComputerUuid(fields.device_id);
    }

    nlohmann::json document = {
        {"format", kTokenFormat},
        {"format_version", kTokenFormatVersion},
        {"client_id", TrimCopy(fields.client_id)},
        {"client_name", TrimCopy(fields.client_name)},
        {"device_type", device_type},
        {"device_id", device_id},
        {"issued_at",
         fields.issued_at.empty() ? NowUtcIso8601() : fields.issued_at},
        {"expires_at", fields.expires_at},
        {"issuer", fields.issuer},
        {"signature",
         {{"alg", signature.alg},
          {"encoding", signature.encoding},
          {"value", signature.value}}},
        {"notes", fields.notes},
    };

    if (document.contains("password") || document.contains("private_key")) {
        throw std::runtime_error("refusing to embed secrets in token");
    }
    return document;
}

std::string PreviewTokenJson(
    const TokenFields& fields,
    const TokenSignature& signature)
{
    return BuildTokenDocument(fields, signature).dump(2);
}

void WriteTokenFile(
    const std::filesystem::path& path,
    const TokenFields& fields,
    const TokenSignature& signature)
{
    const nlohmann::json document = BuildTokenDocument(fields, signature);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write token file: " + path.string());
    }
    output << document.dump(2);
    if (!output) {
        throw std::runtime_error("failed writing token file: " + path.string());
    }
}

} // namespace token_issuer
