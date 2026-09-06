#pragma once

#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace auth_db {

struct TokenFields
{
    std::string client_id;
    std::string client_name;
    std::string device_type;
    std::string device_id;
    std::string signature_meta;
};

TokenFields parseTokenFields(const nlohmann::json& document);
TokenFields loadTokenFields(const std::filesystem::path& token_path);

} // namespace auth_db
