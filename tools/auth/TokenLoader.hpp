#pragma once

#include <string>

namespace auth_db {

struct TokenFields
{
    std::string client_id;
    std::string client_name;
    std::string device_type;
    std::string device_id;
    std::string signature_meta;
};

TokenFields loadTokenFields(const std::string& token_path);

} // namespace auth_db
