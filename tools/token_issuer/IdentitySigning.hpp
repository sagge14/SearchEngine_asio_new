#pragma once

#include <string>
#include <string_view>

namespace token_issuer {

// Canonical AUTHENTICATE_V1 / token identity signing message.
// Bytes: client_id + '\n' + client_name + '\n' + device_type + '\n' + device_id + '\n'
// The final newline is mandatory.
// device_type/device_id and id/name must already be trimmed/normalized by caller.
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

} // namespace token_issuer
