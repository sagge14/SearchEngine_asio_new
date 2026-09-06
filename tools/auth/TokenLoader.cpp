#include "TokenLoader.hpp"

#include "Auth/DeviceIdentity.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace auth_db {
namespace {

std::string normalizeDeviceId(
    const std::string& device_type,
    std::string device_id)
{
    if (device_type == auth::kDeviceTypeUsb) {
        return auth::NormalizeUsbDeviceId(std::move(device_id));
    }
    if (device_type == auth::kDeviceTypeComputer) {
        auto uuid = auth::NormalizeComputerUuid(std::move(device_id));
        if (!uuid) {
            throw std::runtime_error(
                "computer device_id must be a usable SMBIOS UUID");
        }
        return *uuid;
    }
    throw std::runtime_error("device_type must be usb or computer");
}

} // namespace

TokenFields loadTokenFields(const std::filesystem::path& token_path)
{
    std::ifstream input(token_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open token file");
    std::string bytes(65537, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || input.gcount() <= 0 || input.gcount() > 65536)
        throw std::runtime_error("token must be 1..65536 bytes");
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return parseTokenFields(nlohmann::json::parse(bytes));
}

TokenFields parseTokenFields(const nlohmann::json& document)
{
    if (!document.is_object()) {
        throw std::runtime_error("token payload must be a JSON object");
    }
    if (document.value("format", std::string()) != "searchclient-auth-token") {
        throw std::runtime_error(
            "token format must be searchclient-auth-token");
    }
    if (document.value("format_version", 0) != 1) {
        throw std::runtime_error("token format_version must be 1");
    }
    if (document.contains("flash_serial")) {
        throw std::runtime_error(
            "legacy flash_serial tokens are not supported; re-issue a "
            "device_type/device_id token");
    }

    TokenFields fields;
    fields.client_id = auth::TrimCopy(document.value("client_id", std::string()));
    fields.client_name =
        auth::TrimCopy(document.value("client_name", std::string()));
    fields.device_type =
        auth::TrimCopy(document.value("device_type", std::string()));
    fields.device_id = document.value("device_id", std::string());

    if (fields.client_id.empty() || fields.client_name.empty() ||
        fields.device_type.empty() ||
        auth::TrimCopy(fields.device_id).empty())
    {
        throw std::runtime_error(
            "token requires non-empty client_id, client_name, device_type, "
            "device_id");
    }
    fields.device_id = normalizeDeviceId(fields.device_type, fields.device_id);

    if (!document.contains("signature") || !document.at("signature").is_object()) {
        throw std::runtime_error("token requires a signature object");
    }
    const auto& signature = document.at("signature");
    const auto alg = signature.value("alg", std::string());
    if (alg != "RS256") {
        throw std::runtime_error("token signature.alg must be \"RS256\"");
    }
    if (!signature.contains("encoding")) {
        throw std::runtime_error("token signature.encoding is required");
    }
    const auto encoding =
        auth::TrimCopy(signature.value("encoding", std::string()));
    if (encoding.empty()) {
        throw std::runtime_error("token signature.encoding must be non-empty");
    }
    if (encoding != "base64") {
        throw std::runtime_error("token signature.encoding must be \"base64\"");
    }
    const auto value = signature.value("value", std::string());
    if (value.empty()) {
        throw std::runtime_error("token signature.value must be non-empty");
    }
    fields.signature_meta = signature.dump();
    return fields;
}

} // namespace auth_db
