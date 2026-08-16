#include "TokenDefaults.hpp"
#include "TokenAscii.hpp"
#include "TokenDocument.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace token_issuer {
namespace {

void ValidateDefaultsObject(const nlohmann::json& document, const char* source)
{
    if (!document.is_object()) {
        throw std::runtime_error(
            std::string(source) + ": defaults must be a JSON object");
    }

    const char* string_keys[] = {
        "format",
        "client_id",
        "client_name",
        "flash_serial",
        "issued_at",
        "issuer",
        "notes",
    };
    for (const char* key : string_keys) {
        if (!document.contains(key)) {
            continue;
        }
        if (!document.at(key).is_string()) {
            throw std::runtime_error(
                std::string(source) + ": field '" + key + "' must be a string");
        }
        const auto value = document.at(key).get<std::string>();
        if (!IsAsciiTokenField(value)) {
            throw std::runtime_error(
                std::string(source) +
                ": field '" + key +
                "' must be printable ASCII only (no Cyrillic)");
        }
    }

    if (document.contains("expires_at") &&
        !(document.at("expires_at").is_null() ||
          document.at("expires_at").is_string()))
    {
        throw std::runtime_error(
            std::string(source) + ": expires_at must be null or string");
    }
    if (document.contains("expires_at") && document.at("expires_at").is_string()) {
        const auto value = document.at("expires_at").get<std::string>();
        if (!IsAsciiTokenField(value)) {
            throw std::runtime_error(
                std::string(source) +
                ": expires_at must be printable ASCII only");
        }
    }

    if (document.contains("signature")) {
        if (!document.at("signature").is_object()) {
            throw std::runtime_error(
                std::string(source) + ": signature must be an object");
        }
    }
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open defaults: " + path.string());
    }
    nlohmann::json document;
    try {
        input >> document;
    } catch (const std::exception& ex) {
        throw std::runtime_error(
            std::string("defaults JSON parse failed: ") + ex.what());
    }
    ValidateDefaultsObject(document, path.string().c_str());
    return document;
}

nlohmann::json MergeMissingFromBuiltIn(nlohmann::json loaded)
{
    const nlohmann::json builtin = BuiltInTokenDefaults();
    for (auto it = builtin.begin(); it != builtin.end(); ++it) {
        if (!loaded.contains(it.key())) {
            loaded[it.key()] = *it;
        }
    }
    return loaded;
}

} // namespace

nlohmann::json BuiltInTokenDefaults()
{
    return nlohmann::json{
        {"format", kTokenFormat},
        {"format_version", kTokenFormatVersion},
        {"client_id", "C-001"},
        {"client_name", "Ivanov I.I."},
        {"flash_serial", ""},
        {"issued_at", ""},
        {"expires_at", nullptr},
        {"issuer", "auth-server"},
        {"signature",
         {{"alg", "none"}, {"encoding", "base64"}, {"value", ""}}},
        {"notes", ""},
    };
}

nlohmann::json LoadTokenDefaults(
    const std::filesystem::path& exe_dir,
    const std::filesystem::path& explicit_path)
{
    if (!explicit_path.empty()) {
        return MergeMissingFromBuiltIn(ReadJsonFile(explicit_path));
    }

    const std::filesystem::path beside =
        exe_dir / "searchclient-auth-token.defaults.json";
    if (std::filesystem::is_regular_file(beside)) {
        return MergeMissingFromBuiltIn(ReadJsonFile(beside));
    }

    const std::filesystem::path in_data =
        exe_dir / "data" / "searchclient-auth-token.defaults.json";
    if (std::filesystem::is_regular_file(in_data)) {
        return MergeMissingFromBuiltIn(ReadJsonFile(in_data));
    }

    return BuiltInTokenDefaults();
}

std::string GenerateClientId()
{
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto us = duration_cast<microseconds>(now).count();
    std::ostringstream oss;
    oss << "C-" << std::uppercase << std::hex << std::setw(6) << std::setfill('0')
        << (static_cast<unsigned>(us) & 0xFFFFFFu);
    return oss.str();
}

} // namespace token_issuer
