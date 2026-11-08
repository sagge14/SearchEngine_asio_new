#pragma once

#include <string>
#include <string_view>

namespace auth {

// Same bytes as TokenIssuer IdentitySigning (keep in sync).
// client_id + '\n' + client_name + '\n' + device_type + '\n' + device_id + '\n'
// The final newline is mandatory.
inline std::string BuildIdentitySigningMessage(
    std::string_view client_id,
    std::string_view client_name,
    std::string_view device_type,
    std::string_view device_id)
{
    std::string message;
    message.reserve(
        client_id.size() + client_name.size() + device_type.size() +
        device_id.size() + 4);
    message.append(client_id);
    message.push_back('\n');
    message.append(client_name);
    message.push_back('\n');
    message.append(device_type);
    message.push_back('\n');
    message.append(device_id);
    message.push_back('\n');
    return message;
}

} // namespace auth
