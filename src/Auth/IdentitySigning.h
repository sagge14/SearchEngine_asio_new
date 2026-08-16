#pragma once

#include <string>
#include <string_view>

namespace auth {

// Same bytes as TokenIssuer IdentitySigning (keep in sync).
// client_id + '\n' + client_name + '\n' + flash_serial + '\n'
inline std::string BuildIdentitySigningMessage(
    std::string_view client_id,
    std::string_view client_name,
    std::string_view flash_serial)
{
    std::string message;
    message.reserve(
        client_id.size() + client_name.size() + flash_serial.size() + 3);
    message.append(client_id);
    message.push_back('\n');
    message.append(client_name);
    message.push_back('\n');
    message.append(flash_serial);
    message.push_back('\n');
    return message;
}

} // namespace auth
