#include "TokenDocument.hpp"
#include "CryptoStub.hpp"
#include "TokenAscii.hpp"
#include "VolumeSerial.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace token_issuer {

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
    if (NormalizeFlashSerial(fields.flash_serial).empty()) {
        throw std::runtime_error("flash_serial must be non-empty");
    }

    const std::string* strings[] = {
        &fields.client_id,
        &fields.client_name,
        &fields.flash_serial,
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

    nlohmann::json document = {
        {"format", kTokenFormat},
        {"format_version", kTokenFormatVersion},
        {"client_id", TrimCopy(fields.client_id)},
        {"client_name", TrimCopy(fields.client_name)},
        {"flash_serial", NormalizeFlashSerial(fields.flash_serial)},
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
